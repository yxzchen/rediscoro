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
/// - Run the worker_loop coroutine
/// - Serialize all socket operations
/// - Dispatch incoming responses via pipeline
///
/// Structural constraints:
/// - Only one worker_loop coroutine instance at a time
/// - worker_loop runs until CLOSED state
/// - Socket exclusively accessed by either connect() or worker_loop (never both)
/// - External requests enqueued via thread-safe methods
///
/// Thread safety and concurrent operations:
/// - enqueue() can be called from any executor
/// - connect() switches to strand internally for all state mutations
/// - close() can be called from any executor
///
/// Critical invariants:
/// 1. Single worker instance: worker_awaitable_.has_value() == (worker is running)
/// 2. Strand serialization: All state_, socket_, pipeline_ mutations on strand
/// 3. Handshake exclusivity: connect() owns socket during CONNECTING, worker_loop does nothing
/// 4. Request rejection: enqueue() rejects all requests during INIT/CONNECTING
/// 5. Cancel handling: connect() checks cancel_ at each await point
/// 6. Resource cleanup: On failure/close, wait for worker_loop to exit completely
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
/// - OPEN: worker_loop processes requests, connect() is idle
/// - FAILED/RECONNECTING: worker_loop handles reconnection
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
  /// MUST occur on the connection's strand to prevent data races with worker_loop.
  ///
  /// Responsibilities during handshake:
  /// - connect() OWNS the socket and pipeline during CONNECTING state
  /// - connect() sends handshake commands (HELLO/AUTH/SELECT/CLIENT SETNAME) via pipeline
  /// - worker_loop does NOT process requests until state becomes OPEN
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
  ///           worker_loop exited)
  ///
  /// Behavior:
  /// - Starts the background worker_loop (if not already started)
  /// - Switches to connection's strand for all state operations
  /// - Executes TCP connection + RESP3 handshake (HELLO, AUTH, SELECT, CLIENT SETNAME)
  /// - On success: transitions to OPEN state, returns empty error_code
  /// - On failure:
  ///   * Cleans up all resources (closes socket, clears pipeline)
  ///   * Waits for worker_loop to exit (co_await worker_awaitable_)
  ///   * Transitions to CLOSED state
  ///   * Returns error details
  ///
  /// Retry support:
  /// - If state is CLOSED (from previous connection failure), this method will:
  ///   * Reset state to INIT
  ///   * Clear last_error and reconnect_count
  ///   * Restart a new worker_loop instance
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
  /// - Notify worker_loop to wake up (wakeup_.notify())
  /// - Wait for worker_loop to reach CLOSED state (co_await worker_awaitable_)
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
  /// - worker_loop exited
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
  /// Start the background worker loop coroutine (internal use only).
  ///
  /// Semantics:
  /// - Spawns worker_loop() on the connection's strand
  /// - Uses use_awaitable completion token and saves the awaitable in worker_awaitable_
  /// - MUST NOT be called if worker_awaitable_.has_value() is true (worker already running)
  ///
  /// Single-instance guarantee:
  /// - Only one worker_loop instance can run at a time
  /// - Checked by: worker_awaitable_.has_value()
  /// - If worker failed and exited, connect() clears worker_awaitable_ after co_await,
  ///   allowing a new worker to be started on retry
  ///
  /// Called by connect() to ensure worker is running before connection attempt.
  ///
  /// PRE: worker_awaitable_.has_value() == false
  /// POST: worker_awaitable_.has_value() == true
  auto run_worker() -> void;

  /// Main worker loop coroutine.
  /// Runs on the connection's strand until CLOSED.
  ///
  /// Design simplification (CRITICAL):
  /// This loop does NOT handle initial connection. It only processes requests after the
  /// connection reaches OPEN state. This eliminates all handshake/request interleaving logic.
  ///
  /// Responsibilities:
  /// - Process pending user requests (do_write)
  /// - Read responses from server (do_read)
  /// - Handle runtime connection errors and automatic reconnection
  ///
  /// NOT responsible for:
  /// - Initial connection and handshake (handled by connect())
  /// - Processing requests during CONNECTING state (enqueue() rejects them)
  /// - Starting the loop (handled by run_worker())
  ///
  /// Loop structure:
  /// while (!cancel_ && state_ != CLOSED) {
  ///   co_await wakeup_.wait();
  ///
  ///   if (state_ == OPEN) {
  ///     // Normal operation - drain all pending work
  ///     while (has_pending_write() || has_pending_read()) {
  ///       if (has_pending_write()) co_await do_write();
  ///       if (has_pending_read()) co_await do_read();
  ///     }
  ///   }
  ///   else if (state_ == FAILED) {
  ///     // Runtime error - handle reconnection
  ///     co_await do_reconnect_or_close();
  ///   }
  ///   // INIT/CONNECTING/RECONNECTING: do nothing, connect() is in charge
  /// }
  ///
  /// Loop invariant (CRITICAL):
  /// - Each wakeup drains ALL pending work before next wait
  /// - Never suspend while work is available
  /// - This prevents "work queued but loop sleeping" deadlock
  ///
  /// State handling:
  /// - INIT/CONNECTING: No-op, connect() owns the socket
  /// - OPEN: Process requests normally (do_write + do_read)
  /// - FAILED: Cleanup and attempt reconnection (runtime errors only)
  /// - RECONNECTING: No-op during handshake, resume OPEN after success
  /// - CLOSING/CLOSED: Exit loop
  ///
  /// FAILED state handling (runtime errors only):
  /// - IMMEDIATELY stop all normal socket IO (do_read/do_write)
  /// - Do NOT attempt do_read() or do_write() after state_ becomes FAILED
  /// - Close socket and clear all in-flight requests with error via pipeline.clear_all()
  /// - If reconnection is enabled:
  ///   - Remain in FAILED during backoff sleep (no socket IO)
  ///   - Transition to RECONNECTING and attempt TCP connect + handshake (do_reconnect)
  /// - If reconnection is disabled: transition to CLOSED
  /// - Rationale: Once FAILED, the socket is in unknown state; further reads/writes are unsafe.
  ///
  /// Work availability check (when state == OPEN):
  /// - has_pending_write() || has_pending_read()
  auto worker_loop() -> iocoro::awaitable<void>;

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
  ///    - Otherwise: stay in FAILED, worker_loop will sleep then reconnect
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

  // Worker loop notification
  notify_event wakeup_;

  // Worker loop lifecycle
  std::optional<iocoro::awaitable<void>> worker_awaitable_{};  // For close() to co_await

  // Reconnection state
  int reconnect_count_{0};  // Number of reconnection attempts (reset on success)
};

}  // namespace rediscoro::detail

#include <rediscoro/detail/impl/connection.ipp>
