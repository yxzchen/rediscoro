#pragma once

#include <rediscoro/client.hpp>

namespace rediscoro {

inline client::client(iocoro::any_executor ex, config cfg)
  : conn_(std::make_shared<detail::connection>(ex, std::move(cfg))) {
}

inline auto client::connect() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - Start connection
  co_return co_await conn_->start();
}

inline auto client::close() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - Stop connection
  co_return co_await conn_->stop();
}

inline auto client::is_connected() const noexcept -> bool {
  // TODO: Implementation
  return conn_->state() == detail::connection_state::OPEN;
}

inline auto client::state() const noexcept -> detail::connection_state {
  return conn_->state();
}

template <typename T, typename... Args>
auto client::execute(Args&&... args) -> iocoro::awaitable<response_slot<T>> {
  // TODO: Implementation
  // - Create a request from args
  // - Enqueue request to connection
  // - Await pending_response
  // - Return result

  request req{std::forward<Args>(args)...};
  auto pending = conn_->template enqueue<T>(std::move(req));
  co_return co_await pending->wait();
}

}  // namespace rediscoro
