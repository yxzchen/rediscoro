#pragma once

#include <rediscoro/config.hpp>
#include <rediscoro/detail/connection_executor.hpp>
#include <rediscoro/detail/connection_state.hpp>
#include <rediscoro/detail/pending_response.hpp>
#include <rediscoro/detail/pipeline.hpp>
#include <rediscoro/detail/stop_scope.hpp>
#include <rediscoro/error.hpp>
#include <rediscoro/error_info.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/logger.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/resp3/parser.hpp>

#include <iocoro/any_io_executor.hpp>
#include <iocoro/awaitable.hpp>
#include <iocoro/condition_event.hpp>
#include <iocoro/ip/tcp.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rediscoro::detail {

auto make_internal_error(std::exception_ptr ep, std::string_view context) -> error_info;

auto make_internal_error_from_current_exception(std::string_view context) -> error_info;

auto fail_sink_with_current_exception(std::shared_ptr<response_sink> const& sink,
                                      std::string_view context) noexcept -> void;

/// Core Redis connection actor.
///
/// High-level model:
/// - A single background actor (`actor_loop`) runs three strand-bound loops:
///   `write_loop`, `read_loop`, `control_loop`.
/// - All state machine and pipeline mutations are serialized on the connection strand.
/// - Requests are only accepted after a successful `connect()`; there is no buffering/replay
///   across connection generations.
///
/// Concurrency notes:
/// - The socket is full-duplex: at most one in-flight read and one in-flight write are allowed
///   concurrently (enforced by per-direction guards in `do_read()` / `do_write()`).
/// - During `CONNECTING`/`RECONNECTING`, the handshake coroutine drives socket IO directly;
///   the runtime loops are gated on `state_ == OPEN` and will not touch the socket.
/// - `enqueue()` is thread-safe (posts work onto the strand); `connect()`/`close()` switch to
///   the strand internally.
///
/// CLOSED writer rule (CRITICAL):
/// - Only `transition_to_closed()` writes `state_ = CLOSED` (called at actor shutdown).
/// - `close()` requests shutdown (`CLOSING` + cancel + resource release) and then joins the actor.
class connection : public std::enable_shared_from_this<connection> {
 public:
  explicit connection(iocoro::any_io_executor ex, config cfg);
  ~connection() noexcept;

  /// Perform initial connection to Redis server.
  ///
  /// Contract:
  /// - Before `connect()` succeeds, `enqueue()` rejects user requests with `client_errc::not_connected`.
  /// - After it succeeds, the connection is `OPEN` and ready for normal request processing.
  ///
  /// Implementation notes:
  /// - All state mutations happen on the strand (to avoid races with the actor loops).
  /// - Handshake is implemented as a regular pipelined request (HELLO/AUTH/SELECT/CLIENT SETNAME),
  ///   and `do_connect()` drives the corresponding socket IO directly while `state_ != OPEN`.
  /// - On failure, cleanup is unified via `close()` (which joins the actor). `connect()` itself
  ///   does not await the actor directly.
  /// - Automatic reconnection applies only to runtime failures after reaching `OPEN`.
  ///
  /// Thread-safety: Can be called from any executor (switches to strand internally).
  auto connect() -> iocoro::awaitable<expected<void, error_info>>;

  /// Request graceful shutdown.
  ///
  /// Behavior (current "determinism-first" shutdown):
  /// - Request cancellation, set `CLOSING`
  /// - Fail all pending requests, close the socket
  /// - Wake all loops and join the background actor
  /// - `transition_to_closed()` runs exactly once at actor shutdown and writes `CLOSED`
  ///
  /// Thread-safety: Can be called from any executor (switches to strand internally).
  /// Idempotent: safe to call multiple times.
  auto close() -> iocoro::awaitable<void>;

  /// Enqueue a request for execution (fixed-size, heterogenous replies).
  /// Can be called from any executor.
  /// Returns a pending_response that will be completed when all replies arrive.
  ///
  /// State gating: only `OPEN` accepts work; all other states fail immediately.
  /// See `detail/connection_state.hpp` for the exact mapping to error codes.
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
  auto enqueue_impl(request req, std::shared_ptr<response_sink> sink,
                    std::chrono::steady_clock::time_point start) -> void;

  /// Get current connection state (for diagnostics).
  [[nodiscard]] auto state() const noexcept -> connection_state {
    return state_snapshot_.load(std::memory_order_acquire);
  }

 private:
  /// Start the background connection actor (internal use only).
  ///
  /// - Spawns `actor_loop()` on the connection strand.
  /// - Safe to call only when the actor is not running.
  /// - The actor keeps the connection alive via `shared_from_this()` and signals `actor_done_`
  ///   on exit (waited by `close()`).
  ///
  /// Called by `connect()` to ensure the actor is running before attempting a connection.
  auto run_actor() -> void;

  /// Top-level connection actor coroutine.
  /// Runs on the connection's strand until CLOSED.
  ///
  /// Structure:
  /// - write_loop(): flushes pending pipeline writes when `state_ == OPEN`
  /// - read_loop(): performs socket reads when `state_ == OPEN` and delivers parsed messages;
  ///   unsolicited server messages (e.g. PUSH) are treated as an error for now
  /// - control_loop(): owns reconnection/backoff and request-timeout enforcement
  ///
  /// Concurrency constraints (MUST hold):
  /// - All three loops run on the same strand (no parallel executors)
  /// - At most ONE in-flight async_read_some at any time (read_in_flight_)
  /// - At most ONE in-flight async_write_some at any time (write_in_flight_)
  /// - Error handling is centralized: any IO error funnels into handle_error(ec) on strand
  ///
  /// Write-loop gate (CRITICAL):
  /// - write_loop MUST NOT touch the socket unless state_ == OPEN.
  /// - If state_ != OPEN, write_loop waits on write_wakeup_ and retries.
  /// - control_loop is responsible for notify(write_wakeup_) when transitioning to OPEN.
  auto actor_loop() -> iocoro::awaitable<void>;

  /// Write loop (full-duplex direction: write).
  /// Woken by: enqueue(), reconnect success, and internal progress.
  auto write_loop() -> iocoro::awaitable<void>;

  /// Read loop (full-duplex direction: read).
  /// Woken by: transition to `OPEN`, close/cancel, and internal progress.
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
  ///
  /// Returns:
  /// - expected<void, error_info>{}: connection succeeded, state_ = OPEN
  /// - unexpected(error_info): connection failed with specific error
  auto do_connect() -> iocoro::awaitable<expected<void, error_info>>;

  /// Read and parse RESP3 messages from socket.
  ///
  /// Implementation:
  /// - Performs one socket read (`async_read_some`) and then parses as many complete RESP3 values
  ///   as available in the parser buffer.
  /// - Delivers messages into the pipeline in FIFO order; if there is no pending read, the message
  ///   is treated as unsolicited and triggers a runtime error path.
  /// - Leaves partial data in the parser for the next call.
  auto do_read() -> iocoro::awaitable<void>;

  /// Write pending requests to socket.
  ///
  /// Implementation:
  /// - While `state_ == OPEN` and pipeline has pending writes, repeatedly writes some bytes
  ///   (`async_write_some`) and advances the pipeline via `pipeline_.on_write_done()`.
  /// - On successful writes that make reads pending, wakes the read loop.
  auto do_write() -> iocoro::awaitable<void>;

  /// Handle connection error and initiate reconnection.
  ///
  /// Behavior (runtime-only):
  /// - Idempotent guard: ignores errors while already `FAILED`/`RECONNECTING`/`CLOSING`/`CLOSED`.
  /// - Transitions `OPEN -> FAILED`, emits a disconnected event, clears the pipeline, and closes
  ///   socket.
  /// - Reconnection (or deterministic shutdown when disabled) is driven by `control_loop()`.
  ///
  /// Thread-safety: MUST be called from connection strand only
  auto handle_error(error_info ec) -> void;

  /// Perform reconnection loop with exponential backoff.
  ///
  /// Called by `control_loop()` when `state_ == FAILED` and reconnection is enabled:
  /// - Applies immediate attempts + exponential backoff (capped).
  /// - Attempts `do_connect()` under `RECONNECTING`.
  /// - On failure: transitions back to `FAILED`, increments attempt count.
  /// - On success: `do_connect()` sets `OPEN`; attempt counter is reset and connected emitted.
  /// - If close/cancel is requested, returns promptly and lets shutdown proceed.
  auto do_reconnect() -> iocoro::awaitable<void>;

  /// Calculate reconnection delay based on attempt count.
  /// Returns 0 for immediate attempts, jittered/capped backoff otherwise.
  [[nodiscard]] auto calculate_reconnect_delay() const -> std::chrono::milliseconds;

  /// Transition to CLOSED state and cleanup.
  auto transition_to_closed() -> void;

  auto emit_connection_event(connection_event evt) noexcept -> void;

  auto set_state(connection_state next) noexcept -> void {
    state_ = next;
    state_snapshot_.store(next, std::memory_order_release);
  }

 private:
  // Configuration
  config cfg_;

  // Executor management
  connection_executor executor_;

  // Socket
  iocoro::ip::tcp::socket socket_;

  // State machine
  connection_state state_{connection_state::INIT};
  std::atomic<connection_state> state_snapshot_{connection_state::INIT};
  std::uint64_t generation_{0};  // Increments on each successful OPEN transition.

  // Request/response pipeline
  pipeline pipeline_;

  // RESP3 parser
  resp3::parser parser_{};

  // Lifecycle cancellation scope (resettable).
  stop_scope stop_{};

  // Loop notifications (counting wakeups, thread-safe notify)
  iocoro::condition_event write_wakeup_{};
  iocoro::condition_event read_wakeup_{};
  iocoro::condition_event control_wakeup_{};

  // IO in-flight guards (strand-only mutation)
  bool read_in_flight_{false};
  bool write_in_flight_{false};

  // Actor lifecycle
  bool actor_running_{false};
  iocoro::condition_event actor_done_{};

  // Reconnection state
  int reconnect_count_{0};  // Number of reconnection attempts (reset on success)

  // Tracing / diagnostics
  std::uint64_t next_request_id_{1};
};

}  // namespace rediscoro::detail

#include <rediscoro/impl/connection/actor_loops.ipp>
#include <rediscoro/impl/connection/connect.ipp>
#include <rediscoro/impl/connection/core.ipp>
#include <rediscoro/impl/connection/enqueue.ipp>
#include <rediscoro/impl/connection/io.ipp>
#include <rediscoro/impl/connection/reconnect.ipp>
