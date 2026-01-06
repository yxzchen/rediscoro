#pragma once

#include <rediscoro/detail/connection.hpp>
#include <iocoro/bind_executor.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/this_coro.hpp>

namespace rediscoro::detail {

inline connection::connection(iocoro::io_executor ex, config cfg)
  : cfg_(std::move(cfg))
  , executor_(ex)
  , socket_(executor_.get_io_executor()) {
}

inline auto connection::run_actor() -> void {
  // TODO: Implementation
  // - Spawn actor_loop on the connection's strand
  // - Use use_awaitable completion token
  // - Save the awaitable in actor_awaitable_ for close() to use
}

inline auto connection::connect() -> iocoro::awaitable<std::error_code> {
  // TODO: Implementation
  //
  // CRITICAL: All state mutations MUST occur on the connection's strand to prevent
  // data races with the background loops.
  //
  // Single-await rule for actor_awaitable_ (CRITICAL):
  // - iocoro::co_spawn(use_awaitable) supports only one awaiter.
  // - We enforce: ONLY close() awaits actor_awaitable_.
  // - connect() MUST NOT co_await actor_awaitable_ directly.
  // - On connect failure, connect() should co_await close() to perform cleanup and join.
  //
  // 1. Handle CLOSED state - support retry (can do before strand switch)
  //    if (state_ == CLOSED) {
  //      // Previous connection failed and was cleaned up
  //      // Reset state to allow retry
  //      state_ = INIT;
  //      last_error_.reset();
  //      reconnect_count_ = 0;
  //      actor_awaitable_.reset();  // Previous actor has exited
  //    }
  //
  // 2. Check if already connected (idempotency)
  //    if (state_ == OPEN) co_return std::error_code{};
  //
  // 3. Check if currently connecting (concurrent call not supported)
  //    if (state_ == CONNECTING) {
  //      co_return make_error_code(error::already_in_progress);
  //    }
  //
  // 4. Start background actor if not already started
  //    if (!actor_awaitable_.has_value()) {
  //      run_actor();  // Spawns actor_loop, sets actor_awaitable_
  //    }
  //
  // 5. Switch to connection's strand
  //    co_await switch_to(executor_.strand());
  //
  // 6. Check cancellation (handle connect() + close() race)
  //    if (cancel_.is_cancelled()) {
  //      co_return make_error_code(error::operation_aborted);
  //    }
  //
  // 7. Execute initial connection
  //    state_ = CONNECTING;
  //    co_await do_connect();
  //
  // 8. Check cancellation again (do_connect may take time)
  //    if (cancel_.is_cancelled()) {
  //      // close() was called during connection
  //      // Fall through to cleanup (step 10)
  //    }
  //
  // 9. Check result
  //    if (state_ == OPEN) {
  //      co_return std::error_code{};  // Success
  //    }
  //
  // 10. Handle failure - CRITICAL: Cleanup must be unified via close()
  //     // - Determine return error (operation_aborted if cancelled, otherwise last_error_)
  //     // - co_await close();  // performs: cancel + clear pipeline + close socket + join actor
  //     // - After close() completes, connect() may clear actor_awaitable_ to allow retry
  //
  // IMPORTANT: On failure, this method waits for all background coroutines to exit
  // before returning, ensuring clean resource cleanup. This allows retry by calling
  // connect() again.
  co_return std::error_code{};
}

inline auto connection::close() -> iocoro::awaitable<void> {
  // TODO: Implementation
  //
  // 1. Check if already closed (idempotency)
  //    if (state_ == CLOSED) co_return;
  //
  // 2. Check if worker is running
  //    if (!actor_awaitable_.has_value()) {
  //      // Actor never started or already cleaned up
  //      // Just transition to CLOSED
  //      state_ = CLOSED;
  //      co_return;
  //    }
  //
  // 3. Request cancellation
  //    cancel_.request_cancel();
  //
  // 4. Notify loops to wake up and exit
  //    write_wakeup_.notify();
  //    read_wakeup_.notify();
  //    control_wakeup_.notify();
  //
  // 5. Wait for actor_loop to complete
  //    co_await *actor_awaitable_;
  //
  // 5.1. Clear actor_awaitable_ after join (allow restart on retry)
  //      actor_awaitable_.reset();
  //
  // 6. Verify post-condition
  //    REDISCORO_ASSERT(state_ == CLOSED);
  //
  // IMPORTANT: This method waits for all background loops to completely exit before returning.
  // After this method returns, it's safe to destroy the connection object.
  co_return;
}

inline auto connection::enqueue_impl(request req, response_sink* sink) -> void {
  // TODO: Implementation
  //
  // 1. Check current state and reject early if not ready
  //    switch (state_) {
  //      case INIT:
  //      case CONNECTING:
  //        // Connection not established yet
  //        sink->deliver_error(make_error_code(error::not_connected));
  //        return;
  //
  //      case FAILED:
  //        // Connection lost due to runtime error (may be reconnecting in background)
  //        sink->deliver_error(make_error_code(error::connection_lost));
  //        return;
  //
  //      case CLOSING:
  //      case CLOSED:
  //        // Connection shut down
  //        sink->deliver_error(make_error_code(error::connection_closed));
  //        return;
  //
  //      case OPEN:
  //      case RECONNECTING:
  //        // Accept request
  //        break;
  //    }
  //
  // 2. Add request to pipeline
  //    pipeline_.push(std::move(req), sink);
  //
  // 3. Notify write loop to flush ASAP
  //    write_wakeup_.notify();
}

inline auto connection::actor_loop() -> iocoro::awaitable<void> {
  // TODO: Implementation
  //
  // Design philosophy:
  // - This is a top-level "connection actor" that owns lifetime and shutdown coordination.
  // - It spawns three strand-bound coroutines:
  //   * write_loop(): flush pending writes ASAP
  //   * read_loop(): drain pending reads continuously
  //   * control_loop(): centralized state transitions + reconnection
  //
  // Shutdown/joining requirement (CRITICAL):
  // - close() must be able to deterministically wait for all loops to exit before returning.
  // - actor_loop is the single awaitable that close() waits on (actor_awaitable_).
  //
  // Implementation sketch:
  // - co_spawn(executor_.strand().any_executor(), write_loop(), detached);
  // - co_spawn(executor_.strand().any_executor(), read_loop(), detached);
  // - co_spawn(executor_.strand().any_executor(), control_loop(), detached);
  // - Wait for all three loops to exit (via explicit join mechanism; e.g. strand-only counter
  //   + notify_event, or iocoro::when_all if available).
  // - transition_to_closed() at the end.
  co_return;
}

inline auto connection::write_loop() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - co_await write_wakeup_.wait() when no work / not OPEN
  // - while (state_ == OPEN && pipeline_.has_pending_write()) { drain write }
  // - on progress: notify read_wakeup_ if new pending reads become available
  // - on error: handle_error(ec) and notify control_wakeup_
  co_return;
}

inline auto connection::read_loop() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - if (state_ != OPEN || !pipeline_.has_pending_read()) { co_await read_wakeup_.wait(); continue; }
  // - async_read_some + parser + pipeline_.on_message/on_error
  // - on error: handle_error(ec) and notify control_wakeup_
  co_return;
}

inline auto connection::control_loop() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - Centralize state transitions: FAILED -> (sleep) -> RECONNECTING -> OPEN, cancel -> CLOSED
  // - On entering OPEN: notify read_wakeup_ and write_wakeup_
  // - On close(): ensure socket close + pipeline_.clear_all + notify loops to exit
  co_return;
}

inline auto connection::do_connect() -> iocoro::awaitable<void> {
  // TODO: Implementation
  //
  // This method performs TCP connection + RESP3 handshake.
  // On error, it sets state_ = FAILED and last_error_ with appropriate error code.
  //
  // 1. Resolve address
  //    - On DNS failure: set last_error_ = timeout or system error
  //
  // 2. TCP connect with timeout
  //    - co_await async_connect(socket_, endpoint, timeout)
  //    - On timeout: set last_error_ = error::timeout, state_ = FAILED, co_return
  //    - On error: set last_error_ = system error, state_ = FAILED, co_return
  //
  // 3. Check cancel (in case close() was called during connect)
  //    - if (cancel_.is_cancelled()) { state_ = FAILED; co_return; }
  //
  // 4. Send RESP3 handshake commands via pipeline
  //    CRITICAL: Use pipeline_.push() directly, NOT enqueue()
  //    (enqueue() rejects requests in CONNECTING state)
  //
  //    Commands to send:
  //    - HELLO 3 (switch to RESP3 protocol)
  //    - AUTH username password (if configured)
  //    - SELECT database (if database != 0)
  //    - CLIENT SETNAME name (if configured)
  //
  //    Implementation approach:
  //    a) Create pending_response for handshake
  //    b) Build request with all handshake commands
  //    c) pipeline_.push(request, pending_response*)
  //    d) co_await do_write() to send commands
  //    e) co_await do_read() to receive responses
  //    f) co_await pending_response->wait() to get results
  //    g) Validate each response
  //
  //    Error handling:
  //    - If response is -ERR: set last_error_ = error::handshake_failed, state_ = FAILED, co_return
  //    - If timeout: set last_error_ = error::timeout, state_ = FAILED, co_return
  //    - If socket error: set last_error_ = system error, state_ = FAILED, co_return
  //
  // 5. Handshake complete
  //    - state_ = OPEN
  //    - reconnect_count_ = 0 (reset on success)
  //    - co_return
  //
  // NOTE: Handshake uses pipeline for request/response pairing and unified error handling.
  // User requests are still rejected by enqueue() during CONNECTING state.
  co_return;
}

inline auto connection::do_read() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - async_read_some into buffer
  // - Feed data to parser
  // - Parse messages and dispatch to pipeline
  co_return;
}

inline auto connection::do_write() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - Get next_write_buffer from pipeline
  // - async_write to socket
  // - Notify pipeline of bytes written
  co_return;
}

inline auto connection::handle_error(std::error_code ec) -> void {
  // TODO: Implementation
  // - Log error
  // - Transition to FAILED state
  // - Clear pipeline with error
}

inline auto connection::transition_to_closed() -> void {
  // TODO: Implementation
  // - Close socket
  // - Clear pipeline
  // - Set state to CLOSED
}

template <typename... Ts>
auto connection::enqueue(request req) -> std::shared_ptr<pending_response<Ts...>> {
  REDISCORO_ASSERT(req.reply_count() == sizeof...(Ts));
  auto slot = std::make_shared<pending_response<Ts...>>();

  // Thread-safety: enqueue() may be called from any executor/thread.
  // All state_ / pipeline_ mutation must happen on the connection strand.
  executor_.strand().any_executor().post(
    [self = shared_from_this(), req = std::move(req), slot]() mutable {
      self->enqueue_impl(std::move(req), slot.get());
    }
  );

  return slot;
}

template <typename T>
auto connection::enqueue_dynamic(request req) -> std::shared_ptr<pending_dynamic_response<T>> {
  auto slot = std::make_shared<pending_dynamic_response<T>>(req.reply_count());

  // Thread-safety: enqueue_dynamic() may be called from any executor/thread.
  // All state_ / pipeline_ mutation must happen on the connection strand.
  executor_.strand().any_executor().post(
    [self = shared_from_this(), req = std::move(req), slot]() mutable {
      self->enqueue_impl(std::move(req), slot.get());
    }
  );

  return slot;
}

}  // namespace rediscoro::detail
