#pragma once

#include <rediscoro/detail/pending_response.hpp>

namespace rediscoro::detail {

template <typename... Ts>
auto pending_response<Ts...>::do_deliver(resp3::message msg) -> void {
  REDISCORO_ASSERT(!result_.has_value());
  if (result_.has_value()) {
    return;
  }

  builder_.accept(std::move(msg));
  if (builder_.done()) {
    result_ = builder_.take_results();
    event_.notify();
  }
}

template <typename... Ts>
auto pending_response<Ts...>::do_deliver_error(resp3::error err) -> void {
  REDISCORO_ASSERT(!result_.has_value());
  if (result_.has_value()) {
    return;
  }

  builder_.accept(err);
  if (builder_.done()) {
    result_ = builder_.take_results();
    event_.notify();
  }
}

template <typename... Ts>
auto pending_response<Ts...>::wait() -> iocoro::awaitable<response<Ts...>> {
  co_await event_.wait();
  REDISCORO_ASSERT(result_.has_value());
  co_return std::move(*result_);
}

template <typename T>
auto pending_dynamic_response<T>::do_deliver(resp3::message msg) -> void {
  REDISCORO_ASSERT(!result_.has_value());
  if (result_.has_value()) {
    return;
  }

  builder_.accept(std::move(msg));
  if (builder_.done()) {
    result_ = builder_.take_results();
    event_.notify();
  }
}

template <typename T>
auto pending_dynamic_response<T>::do_deliver_error(resp3::error err) -> void {
  REDISCORO_ASSERT(!result_.has_value());
  if (result_.has_value()) {
    return;
  }

  builder_.accept(err);
  if (builder_.done()) {
    result_ = builder_.take_results();
    event_.notify();
  }
}

template <typename T>
auto pending_dynamic_response<T>::wait() -> iocoro::awaitable<dynamic_response<T>> {
  co_await event_.wait();
  REDISCORO_ASSERT(result_.has_value());
  co_return std::move(*result_);
}

}  // namespace rediscoro::detail
