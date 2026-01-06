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
/// Responsibilities:
/// - Manage socket lifecycle on a single strand
/// - Run the worker_loop coroutine
/// - Serialize all socket operations
/// - Dispatch incoming responses via pipeline
///
/// Structural constraints:
/// - Only one worker_loop coroutine
/// - worker_loop runs until CLOSED state
/// - Socket only accessed within worker_loop
/// - External requests enqueued via thread-safe methods
///
/// Thread safety:
/// - enqueue() can be called from any executor
/// - All other methods run on the connection's strand
class connection : public std::enable_shared_from_this<connection> {
public:
  explicit connection(iocoro::io_executor ex, config cfg);

  /// Perform initial connection to Redis server.
  ///
  /// Behavior:
  /// - Starts the background worker_loop (if not already started)
  /// - Executes TCP connection + RESP3 handshake (HELLO, AUTH, SELECT, CLIENT SETNAME)
  /// - On success: transitions to OPEN state, returns empty error_code
  /// - On failure:
  ///   * Cleans up all resources (closes socket, clears pipeline)
  ///   * Waits for all background coroutines to exit
  ///   * Transitions to CLOSED state
  ///   * Returns error details
  ///
  /// IMPORTANT: This method does NOT trigger automatic reconnection.
  /// Automatic reconnection only applies to connection failures AFTER reaching OPEN state.
  ///
  /// Thread-safety: Can be called from any executor
  /// Idempotency: Can be called multiple times (subsequent calls return cached result)
  auto connect() -> iocoro::awaitable<std::error_code>;

  /// Request graceful shutdown.
  /// Can be called from any executor.
  ///
  /// Behavior:
  /// - Set cancel flag
  /// - Notify worker_loop to wake up
  /// - Wait for worker_loop to reach CLOSED state
  /// - If called during RECONNECTING, interrupts reconnection
  ///
  /// Implementation:
  /// - Uses co_await on worker_awaitable_ (set during start())
  /// - If worker_awaitable_ not available (destructor case), best-effort
  auto close() -> iocoro::awaitable<void>;

  /// Enqueue a request for execution (fixed-size, heterogenous replies).
  /// Can be called from any executor.
  /// Returns a pending_response that will be completed when all replies arrive.
  ///
  /// Behavior by state:
  /// - INIT, CONNECTING, OPEN, RECONNECTING: Request accepted and queued
  /// - FAILED: Request is rejected immediately (client_error: connection_error)
  /// - CLOSING, CLOSED: Request is rejected immediately (client_error: connection_closed)
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
  /// - Uses detached completion token (fire-and-forget)
  /// - Can only be called once (protected by std::call_once)
  ///
  /// Called by connect() to ensure worker is running before connection attempt.
  auto run_worker() -> void;

  /// Main worker loop coroutine.
  /// Runs on the connection's strand until CLOSED.
  ///
  /// Responsibilities:
  /// - Process pending requests (do_write)
  /// - Read responses from server (do_read)
  /// - Handle runtime connection errors and automatic reconnection
  ///
  /// NOT responsible for:
  /// - Initial connection (handled by connect())
  /// - Starting the loop (handled by run_worker())
  ///
  /// Loop invariant (CRITICAL):
  /// - Each wakeup drains ALL pending work before next wait
  /// - Never suspend while work is available
  /// - This prevents "work queued but loop sleeping" deadlock
  ///
  /// Drain strategy (OPEN state):
  /// 1. Wait for wakeup notification (may have multiple counts)
  /// 2. Loop until no work remains:
  ///    - Write all pending requests (until socket blocks)
  ///    - Read all available responses (until socket blocks or no pending reads)
  /// 3. Only then wait for next wakeup
  ///
  /// Drain strategy (FAILED state - runtime errors only):
  /// - IMMEDIATELY stop all normal socket IO (do_read/do_write)
  /// - Do NOT attempt do_read() or do_write() after state_ becomes FAILED
  /// - Close socket and clear all in-flight requests with error via pipeline.clear_all()
  /// - If reconnection is enabled:
  ///   - Remain in FAILED during backoff sleep (no socket IO)
  ///   - Transition to RECONNECTING and attempt TCP connect + handshake (do_reconnect)
  /// - If reconnection is disabled: transition to CLOSED
  /// - Rationale: Once FAILED, the socket is in unknown state; further reads/writes are unsafe.
  ///
  /// State transition on error:
  /// - Any do_read/do_write error → call handle_error(ec)
  /// - handle_error() sets state = FAILED
  /// - Worker loop MUST NOT continue do_read/do_write in the same drain pass after FAILED
  /// - Next drain pass sees FAILED → cleanup + optional reconnection path only
  ///
  /// Work availability check (when state == OPEN):
  /// - has_pending_write() || has_pending_read() || cancel_requested()
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
  std::once_flag worker_start_flag_;  // Ensures run_worker() is called exactly once
  std::optional<iocoro::awaitable<void>> worker_awaitable_{};  // For close() to co_await

  // Reconnection state
  int reconnect_count_{0};  // Number of reconnection attempts (reset on success)
};

}  // namespace rediscoro::detail

#include <rediscoro/detail/impl/connection.ipp>
