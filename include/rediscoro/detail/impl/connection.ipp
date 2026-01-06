#pragma once

#include <rediscoro/detail/connection.hpp>
#include <iocoro/bind_executor.hpp>

namespace rediscoro::detail {

inline connection::connection(iocoro::io_executor ex, config cfg)
  : cfg_(std::move(cfg))
  , executor_(ex)
  , socket_(executor_.get_io_executor()) {
}

inline auto connection::run_worker() -> void {
  // TODO: Implementation
  // - Spawn worker_loop on the connection's strand
  // - Use use_awaitable completion token
  // - Save the awaitable in worker_awaitable_ for close() to use
}

inline auto connection::connect() -> iocoro::awaitable<std::error_code> {
  // TODO: Implementation
  //
  // 1. Handle CLOSED state - support retry
  //    if (state_ == CLOSED) {
  //      // Previous connection failed and was cleaned up
  //      // Reset state to allow retry
  //      state_ = INIT;
  //      last_error_.reset();
  //      reconnect_count_ = 0;
  //      worker_awaitable_.reset();  // Previous worker has exited
  //    }
  //
  // 2. Check if already connected (idempotency)
  //    if (state_ == OPEN) co_return std::error_code{};
  //
  // 3. Check if currently connecting (concurrent call not supported)
  //    if (state_ == CONNECTING) {
  //      co_return make_error_code(error::connection_error);
  //    }
  //
  // 4. Start worker_loop if not already started
  //    if (!worker_awaitable_.has_value()) {
  //      run_worker();  // Spawns worker_loop, sets worker_awaitable_
  //    }
  //
  // 5. Switch to connection's strand
  //    co_await switch_to(executor_.strand());
  //
  // 6. Execute initial connection
  //    state_ = CONNECTING;
  //    co_await do_connect();
  //
  // 7. Check result
  //    if (state_ == OPEN) {
  //      co_return std::error_code{};  // Success
  //    }
  //
  // 8. Handle failure - CRITICAL: Clean up all resources and wait for worker exit
  //    // - Set cancel flag to signal worker_loop to exit
  //    // - Close socket
  //    // - Clear all pending requests
  //    // - Notify worker to wake up and exit
  //    // - Wait for worker_loop to complete (co_await worker_awaitable_)
  //    // - Transition to CLOSED
  //    // - Return error
  //
  // IMPORTANT: On failure, this method waits for all background coroutines to exit
  // before returning, ensuring clean resource cleanup. This allows retry by calling
  // connect() again.
  co_return std::error_code{};
}

inline auto connection::close() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - Request cancellation
  // - Notify worker loop
  // - Wait for CLOSED state
  co_return;
}

inline auto connection::enqueue_impl(request req, response_sink* sink) -> void {
  // TODO: Implementation
  // - Add request to pipeline (thread-safe)
  // - Notify worker loop via wakeup_.notify()
}

inline auto connection::worker_loop() -> iocoro::awaitable<void> {
  // TODO: Implementation
  //
  // Main loop: Process requests and responses while connection is active
  // - while (!cancelled && state != CLOSED):
  //   - wait for wakeup_
  //   - if cancelled: break
  //   - if state == FAILED: handle runtime reconnection
  //     * Only triggered by runtime IO errors AFTER reaching OPEN state
  //     * NOT triggered by initial connection failure (handled by connect())
  //     * If reconnection enabled: co_await do_reconnect()
  //     * If reconnection disabled: break
  //   - if has_pending_write: do_write()
  //   - if has_pending_read: do_read()
  //
  // Cleanup:
  // - transition_to_closed()
  //
  // NOTE: Initial connection is NOT handled here, it's handled by connect()
  co_return;
}

inline auto connection::do_connect() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // 1. Resolve address
  // 2. Connect with timeout and retry
  // 3. Transition to OPEN state
  // 4. Send handshake commands via pipeline:
  //    - Create pending_response for each handshake command
  //    - Push to pipeline: HELLO 3, AUTH (if needed), SELECT (if needed), CLIENT SETNAME (if needed)
  //    - Await each response
  //    - If any fails, transition to FAILED
  // 5. Handshake complete, ready for user requests
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
  enqueue_impl(std::move(req), slot.get());
  return slot;
}

template <typename T>
auto connection::enqueue_dynamic(request req) -> std::shared_ptr<pending_dynamic_response<T>> {
  auto slot = std::make_shared<pending_dynamic_response<T>>(req.reply_count());
  enqueue_impl(std::move(req), slot.get());
  return slot;
}

}  // namespace rediscoro::detail
