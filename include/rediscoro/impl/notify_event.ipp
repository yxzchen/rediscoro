#pragma once

#include <rediscoro/detail/notify_event.hpp>

#include <iocoro/this_coro.hpp>

namespace rediscoro::detail {

struct notify_event::awaiter {
  notify_event* self;

  auto await_ready() const noexcept -> bool {
    return false;
  }

  auto await_suspend(std::coroutine_handle<> h) -> bool {
    std::lock_guard lk{self->mutex_};

    // If there is already a pending notification, consume it and do not suspend.
    auto cnt = self->count_.load(std::memory_order_acquire);
    if (cnt > 0) {
      self->count_.fetch_sub(1, std::memory_order_acq_rel);
      return false;  // resume immediately
    }

    // Otherwise, register waiter atomically with the decision to suspend.
    self->awaiting_ = h;
    self->executor_ = iocoro::this_coro::get_executor();
    return true;  // suspend
  }

  auto await_resume() const noexcept -> void {
    // Nothing to return
  }
};

inline auto notify_event::wait() -> iocoro::awaitable<void> {
  co_await awaiter{this};
}

inline auto notify_event::notify() -> void {
  std::optional<std::coroutine_handle<>> to_resume{};
  iocoro::any_executor ex{};

  {
    std::lock_guard lk{mutex_};
    if (awaiting_) {
      to_resume = *awaiting_;
      ex = executor_;
      awaiting_.reset();
    } else {
      count_.fetch_add(1, std::memory_order_acq_rel);
      return;
    }
  }

  // Post resumption to the captured executor; never resume inline.
  ex.post([h = *to_resume]() mutable {
    h.resume();
  });
}

}  // namespace rediscoro::detail
