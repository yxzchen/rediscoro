#pragma once

#include <rediscoro/config.hpp>
#include <rediscoro/detail/cancel_source.hpp>
#include <rediscoro/detail/connection_state.hpp>
#include <rediscoro/detail/connection_executor.hpp>
#include <rediscoro/detail/notify_event.hpp>
#include <rediscoro/detail/pending_response.hpp>
#include <rediscoro/detail/pipeline.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/resp3/parser.hpp>

#include <iocoro/awaitable.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/ip/tcp.hpp>

#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace rediscoro::detail {

/// Core connection actor.
///
/// Design philosophy (CRITICAL):
/// This class enforces a clean separation between connection establishment and normal operation:
/// - BEFORE connect() succeeds → NO user requests accepted (enqueue returns not_connected)
/// - AFTER connect() succeeds → Normal request processing begins
/// This eliminates all handshake/request interleaving complexity.
///
/// Responsibilities:
/// - Manage socket lifecycle on a single strand
/// - Run the background connection actor (read_loop / write_loop / control_loop)
/// - Serialize all state_ / pipeline_ mutations on a single strand
/// - Dispatch incoming responses via pipeline
///
/// Structural constraints:
/// - Only one background actor instance at a time
/// - The actor runs until CLOSED state
/// - Socket exclusively accessed by either:
///   - connect() handshake (during CONNECTING), OR
///   - IO loops (during OPEN), OR
///   - reconnection handshake (during RECONNECTING, driven by control_loop)
/// - External requests enqueued via thread-safe methods
///
/// Thread safety and concurrent operations:
/// - enqueue() can be called from any executor
/// - connect() switches to strand internally for all state mutations
/// - close() can be called from any executor
///
/// Critical invariants:
/// 1. Single actor instance: actor_awaitable_.has_value() == (actor is running)
/// 2. Strand serialization: All state_, socket_, pipeline_ mutations on strand
/// 3. Handshake exclusivity: connect() owns socket during CONNECTING, IO loops do nothing
/// 4. Request rejection: enqueue() rejects all requests during INIT/CONNECTING
/// 5. Cancel handling: connect() checks cancel_ at each await point
/// 6. Resource cleanup: On failure/close, wait for background actor to exit completely
/// 7. Retry support: After CLOSED, connect() can reset and retry
///
/// State transition rules:
/// - connect() success: INIT → CONNECTING → OPEN
/// - connect() failure: INIT → CONNECTING → CLOSED (with full cleanup)
/// - Runtime error: OPEN → FAILED → RECONNECTING (if enabled) OR CLOSED
/// - close() called: any state → CLOSED
///
/// Ownership during states:
/// - INIT: No owner (waiting for connect())
/// - CONNECTING: connect() owns socket exclusively
/// - OPEN: read_loop/write_loop process IO, connect() is idle
/// - FAILED/RECONNECTING: control_loop handles reconnection policy and transitions
/// - CLOSED: No owner
class connection : public std::enable_shared_from_this<connection> {
public:
  explicit connection(iocoro::io_executor ex, config cfg);

  /// Perform initial connection to Redis server.
  ///
  /// Design philosophy:
  /// This method establishes a clean boundary: BEFORE connect() completes successfully,
  /// NO user requests are accepted. This simplifies the entire system by ensuring that
  /// handshake and normal operation never overlap.
  ///
  /// Semantics (CRITICAL):
  /// This is a "strand-serialized operation". All state mutations (state_, socket_, pipeline_)
  /// MUST occur on the connection's strand to prevent data races with the background loops.
  ///
  /// Responsibilities during handshake:
  /// - connect() OWNS the socket and pipeline during CONNECTING state
  /// - connect() sends handshake commands (HELLO/AUTH/SELECT/CLIENT SETNAME) via pipeline
  /// - read_loop/write_loop do NOT perform socket IO until state becomes OPEN
  /// - enqueue() REJECTS user requests during INIT/CONNECTING states (error::not_connected)
  /// - This exclusive ownership eliminates all handshake/user-request interleaving complexity
  ///
  /// Why handshake uses pipeline:
  /// - Pipeline provides request/response pairing mechanism
  /// - Reuses existing RESP3 encoding/decoding logic
  /// - Unified error handling for all commands
  /// - connect() can directly call pipeline_.push() without going through enqueue()
  ///
  /// Post-condition guarantee:
  /// When this method returns, the connection is in one of two states:
  /// - OPEN: Connection established, handshake complete, ready for user requests
  /// - CLOSED: Connection failed and all resources cleaned up (socket closed, pipeline cleared,
  ///           background actor exited)
  ///
  /// Behavior:
  /// - Starts the background connection actor (if not already started)
  /// - Switches to connection's strand for all state operations
  /// - Executes TCP connection + RESP3 handshake (HELLO, AUTH, SELECT, CLIENT SETNAME)
  /// - On success: transitions to OPEN state, returns empty error_code
  /// - On failure:
  ///   * Cleans up all resources (closes socket, clears pipeline)
  ///   * Waits for actor to exit (co_await actor_awaitable_)
  ///   * Transitions to CLOSED state
  ///   * Returns error details
  ///
  /// Retry support:
  /// - If state is CLOSED (from previous connection failure), this method will:
  ///   * Reset state to INIT
  ///   * Clear last_error and reconnect_count
  ///   * Restart a new background actor instance
  ///   * Retry the connection
  /// - This allows retrying connection without creating a new connection object
  ///
  /// Concurrent call handling:
  /// - connect() + connect(): If state is CONNECTING, returns already_in_progress
  /// - connect() + close(): close() wins, connect() checks cancel_ at each await point
  ///                        and returns operation_aborted if cancelled
  ///
  /// IMPORTANT: This method does NOT trigger automatic reconnection.
  /// Automatic reconnection only applies to connection failures AFTER reaching OPEN state.
  ///
  /// Possible error codes:
  /// - std::error_code{} (empty): Success, connection is OPEN
  /// - error::already_in_progress: Another connect() is already in progress
  /// - error::operation_aborted: close() was called during connection
  /// - error::timeout: TCP connection or handshake timed out
  /// - error::handshake_failed: RESP3 handshake failed (HELLO/AUTH/SELECT)
  /// - system error codes: TCP connection failed, DNS resolution failed, etc.
  ///
  /// Thread-safety: Can be called from any executor (switches to strand internally)
  auto connect() -> iocoro::awaitable<std::error_code>;

  /// Request graceful shutdown.
  ///
  /// Behavior:
  /// - Set cancel flag (cancel_.request_cancel())
  /// - Notify all loops to wake up (write_wakeup_/read_wakeup_/control_wakeup_)
  /// - Wait for background actor to reach CLOSED state (co_await actor_awaitable_)
  /// - If called during RECONNECTING, interrupts reconnection
  ///
  /// Concurrent call handling:
  /// - close() + connect(): close() wins, connect() detects cancel_ and returns operation_aborted
  /// - close() + close(): Idempotent, second call is no-op if already closed
  ///
  /// Post-condition:
  /// - state_ == CLOSED
  /// - Socket closed
  /// - All pending requests cleared
  /// - All background loops exited
  ///
  /// Thread-safety: Can be called from any executor
  /// Idempotency: Safe to call multiple times
  auto close() -> iocoro::awaitable<void>;

  /// Enqueue a request for execution (fixed-size, heterogenous replies).
  /// Can be called from any executor.
  /// Returns a pending_response that will be completed when all replies arrive.
  ///
  /// IMPORTANT: Requests can only be enqueued AFTER connect() succeeds.
  /// This design ensures clean separation between connection establishment and normal operation.
  ///
  /// Behavior by state:
  /// - INIT, CONNECTING: Request rejected immediately (error::not_connected)
  ///                     User must wait for connect() to complete
  /// - OPEN, RECONNECTING: Request accepted and queued
  /// - FAILED: Request rejected immediately (error::connection_lost)
  ///           Connection lost due to runtime error, automatic reconnection may be in progress
  /// - CLOSING, CLOSED: Request rejected immediately (error::connection_closed)
  ///                    Connection has been shut down
  template <typename... Ts>
  auto enqueue(request req) -> std::shared_ptr<pending_response<Ts...>>;

  /// Enqueue a pipeline request (homogeneous reply type).
  ///
  /// Contract:
  /// - Expected reply count is req.reply_count() at enqueue time.
  template <typename T>
  auto enqueue_dynamic(request req) -> std::shared_ptr<pending_dynamic_response<T>>;

  /// Internal enqueue implementation (type-erased).
  /// MUST be called from connection strand.
  auto enqueue_impl(request req, response_sink* sink) -> void;

  /// Get current connection state (for diagnostics).
  [[nodiscard]] auto state() const noexcept -> connection_state {
    return state_;
  }

  /// Last connection error observed (if any).
  ///
  /// This is set when the connection transitions through FAILED due to an IO/handshake error.
  /// When automatic reconnection is disabled, the connection will still transition to CLOSED
  /// for deterministic cleanup, but the last_error remains available for diagnostics.
  [[nodiscard]] auto last_error() const noexcept -> std::optional<std::error_code> {
    return last_error_;
  }

private:
  /// Start the background connection actor (internal use only).
  ///
  /// Semantics:
  /// - Spawns actor_loop() on the connection's strand
  /// - Uses use_awaitable completion token and saves the awaitable in actor_awaitable_
  /// - MUST NOT be called if actor_awaitable_.has_value() is true (actor already running)
  ///
  /// Single-instance guarantee:
  /// - Only one actor_loop instance can run at a time
  /// - Checked by: actor_awaitable_.has_value()
  /// - If actor exited, connect() clears actor_awaitable_ after co_await,
  ///   allowing a new actor to be started on retry
  ///
  /// Called by connect() to ensure the actor is running before connection attempt.
  ///
  /// PRE: actor_awaitable_.has_value() == false
  /// POST: actor_awaitable_.has_value() == true
  auto run_actor() -> void;

  /// Top-level connection actor coroutine.
  /// Runs on the connection's strand until CLOSED.
  ///
  /// Why the actor is split into three coroutines (CRITICAL):
  /// - A single worker_loop that serializes do_write/do_read introduces structural write latency:
  ///   when the coroutine is suspended in read (or any other await point), a new enqueue cannot
  ///   trigger an immediate write flush even if the socket is writable.
  /// - TCP sockets are full-duplex: write readiness and read readiness are independent.
  /// - Correctness goal: "writes should flush ASAP" AND "responses should be drained continuously".
  ///   This requires independent progress of read vs write.
  ///
  /// Actor structure:
  /// - write_loop(): only writes; flushes whenever pipeline has pending write
  /// - read_loop(): only reads; drains whenever pipeline has pending read
  /// - control_loop(): owns state transitions (FAILED/RECONNECTING/CLOSING/CLOSED) and reconnection
  ///
  /// Concurrency constraints (MUST hold):
  /// - All three loops run on the same strand (no parallel executors)
  /// - At most ONE in-flight async_read_some at any time (read_in_flight_)
  /// - At most ONE in-flight async_write_some at any time (write_in_flight_)
  /// - Error handling is centralized: any IO error funnels into handle_error(ec) on strand
  auto actor_loop() -> iocoro::awaitable<void>;

  /// Write loop (full-duplex direction: write).
  /// Woken by: enqueue(), reconnect success, and internal progress.
  auto write_loop() -> iocoro::awaitable<void>;

  /// Read loop (full-duplex direction: read).
  /// Woken by: first transition to pending-read, reconnect success, and internal progress.
  auto read_loop() -> iocoro::awaitable<void>;

  /// Control loop: centralized state transitions and reconnection policy.
  /// Woken by: handle_error(), connect()/handshake completion, close(), timers.
  auto control_loop() -> iocoro::awaitable<void>;

  /// Connect to Redis server with timeout and retry.
  /// After TCP connection succeeds, sends handshake commands:
  /// - HELLO 3 (switch to RESP3)
  /// - AUTH (if username/password configured)
  /// - SELECT (if database != 0)
  /// - CLIENT SETNAME (if client_name configured)
  ///
  /// These are sent as regular requests using the existing pipeline,
  /// not as special handshake methods.
  auto do_connect() -> iocoro::awaitable<void>;

  /// Read and parse RESP3 messages from socket.
  ///
  /// Completion semantics:
  /// - Attempts to read at least ONE complete RESP3 message
  /// - May read multiple messages if available in socket buffer
  /// - Returns when:
  ///   a) At least one message parsed AND socket would block (EAGAIN)
  ///   b) No more pending reads in pipeline (optimization)
  ///   c) Error occurred
  /// - NEVER returns without either reading a message or encountering error/block
  auto do_read() -> iocoro::awaitable<void>;

  /// Write pending requests to socket.
  ///
  /// Completion semantics:
  /// - Attempts to write ALL pending request buffers
  /// - Returns when:
  ///   a) All pending writes completed (pipeline.has_pending_write() == false)
  ///   b) Socket would block (EAGAIN)
  ///   c) Error occurred
  /// - Partial writes are tracked via pipeline.on_write_done()
  /// - May be called again immediately if socket becomes writable
  auto do_write() -> iocoro::awaitable<void>;

  /// Handle connection error and initiate reconnection.
  ///
  /// Behavior:
  /// 1. Guard: if already in error-handling state, return
  /// 2. Set state = FAILED
  /// 3. Close socket
  /// 4. Clear all pending requests with error via pipeline.clear_all()
  /// 5. If reconnection enabled:
  ///    - If immediate_attempts not exhausted: set state = RECONNECTING immediately
  ///    - Otherwise: stay in FAILED, control_loop will sleep then reconnect
  /// 6. If reconnection disabled: set state = CLOSED
  ///
  /// Thread-safety: MUST be called from connection strand only
  auto handle_error(std::error_code ec) -> void;

  /// Perform reconnection loop with exponential backoff.
  ///
  /// Strategy:
  /// - Infinite loop until success or cancel
  /// - First N attempts: immediate (no delay)
  /// - After N attempts: exponential backoff (delay doubles each time)
  /// - Capped at max_delay, then retry at max_delay indefinitely
  ///
  /// State transitions:
  /// - On success: state = OPEN, reconnect_count_ = 0, returns
  /// - On failure: reconnect_count_++, calculate delay, sleep, retry
  /// - On cancel: state = CLOSED, pipeline.clear_all(), returns
  ///
  /// Return semantics:
  /// - Returns only when: (a) success (state=OPEN) or (b) cancelled (state=CLOSED)
  /// - Never returns with state=FAILED or state=RECONNECTING
  auto do_reconnect() -> iocoro::awaitable<void>;

  /// Calculate reconnection delay based on attempt count.
  /// Returns 0 for immediate attempts, exponential backoff otherwise.
  [[nodiscard]] auto calculate_reconnect_delay() const -> std::chrono::milliseconds;

  /// Transition to CLOSED state and cleanup.
  auto transition_to_closed() -> void;

private:
  // Configuration
  config cfg_;

  // Executor management
  connection_executor executor_;

  // Socket
  iocoro::ip::tcp::socket socket_;

  // State machine
  connection_state state_{connection_state::INIT};

  std::optional<std::error_code> last_error_{};

  // Request/response pipeline
  pipeline pipeline_;

  // RESP3 parser
  resp3::parser parser_{};

  // Cancellation
  cancel_source cancel_;

  // Loop notifications (counting wakeups, thread-safe notify)
  notify_event write_wakeup_{};
  notify_event read_wakeup_{};
  notify_event control_wakeup_{};

  // IO in-flight guards (strand-only mutation)
  bool read_in_flight_{false};
  bool write_in_flight_{false};

  // Actor lifecycle
  std::optional<iocoro::awaitable<void>> actor_awaitable_{};  // For close() to co_await

  // Reconnection state
  int reconnect_count_{0};  // Number of reconnection attempts (reset on success)
};

}  // namespace rediscoro::detail

#include <rediscoro/detail/impl/connection.ipp>
