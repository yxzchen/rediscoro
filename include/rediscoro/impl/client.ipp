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
auto client::exec(Args&&... args) -> iocoro::awaitable<response<T>> {
  request req{std::forward<Args>(args)...};
  auto pending = conn_->template enqueue<T>(std::move(req));
  co_return co_await pending->wait();
}

template <typename... Ts>
auto client::exec(request req) -> iocoro::awaitable<response<Ts...>> {
  auto pending = conn_->template enqueue<Ts...>(std::move(req));
  co_return co_await pending->wait();
}

template <typename T>
auto client::exec_dynamic(request req) -> iocoro::awaitable<dynamic_response<T>> {
  auto pending = conn_->template enqueue_dynamic<T>(std::move(req));
  co_return co_await pending->wait();
}

}  // namespace rediscoro
