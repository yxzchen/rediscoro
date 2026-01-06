#pragma once

#include <rediscoro/detail/connection.hpp>
#include <iocoro/bind_executor.hpp>

namespace rediscoro::detail {

inline connection::connection(iocoro::io_executor ex, config cfg)
  : cfg_(std::move(cfg))
  , executor_(ex)
  , socket_(executor_.get_io_executor()) {
}

inline auto connection::start() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - Spawn worker_loop on the strand
  // - co_spawn(executor_.get(), worker_loop(), use_awaitable)
  co_return;
}

inline auto connection::stop() -> iocoro::awaitable<void> {
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
  // - do_connect()
  // - while (state_ == OPEN):
  //   - wait for wakeup_
  //   - if cancelled: break
  //   - if has_pending_write: do_write()
  //   - if has_pending_read: do_read()
  // - transition_to_closed()
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
