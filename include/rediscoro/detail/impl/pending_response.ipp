#pragma once

#include <rediscoro/detail/pending_response.hpp>
#include <rediscoro/adapter/adapt.hpp>

namespace rediscoro::detail {

template <typename T>
auto pending_response<T>::do_deliver(resp3::message msg) -> void {
  // Prevent double-delivery (pipeline bug)
  REDISCORO_ASSERT(!result_.has_value() && "deliver() called twice - pipeline bug!");
  if (result_.has_value()) {
    return;  // Defensive: ignore in release builds
  }

  // Adapt message to type T
  auto adapted = adapter::adapt<T>(msg);
  if (adapted) {
    result_ = response_slot<T>{std::move(*adapted)};
  } else {
    result_ = response_slot<T>{unexpected(response_error{std::move(adapted.error())})};
  }

  // Notify waiting coroutine (on its executor)
  event_.notify();
}

template <typename T>
auto pending_response<T>::do_deliver_error(resp3::error err) -> void {
  // Prevent double-delivery (pipeline bug)
  REDISCORO_ASSERT(!result_.has_value() && "deliver_error() called twice - pipeline bug!");
  if (result_.has_value()) {
    return;  // Defensive: ignore in release builds
  }

  // Store error
  result_ = response_slot<T>{unexpected(response_error{err})};

  // Notify waiting coroutine (on its executor)
  event_.notify();
}

template <typename T>
auto pending_response<T>::wait() -> iocoro::awaitable<response_slot<T>> {
  // TODO: Implementation
  // - Wait on event
  // - Return result

  co_await event_.wait();

  // Result must be set at this point
  REDISCORO_ASSERT(result_.has_value());
  co_return std::move(*result_);
}

}  // namespace rediscoro::detail
