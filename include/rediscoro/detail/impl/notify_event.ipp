#pragma once

#include <rediscoro/detail/notify_event.hpp>
#include <iocoro/this_coro.hpp>

namespace rediscoro::detail {

struct notify_event::awaiter {
  notify_event* self;

  auto await_ready() const noexcept -> bool {
    // TODO: Check if already notified
    return false;
  }

  auto await_suspend(std::coroutine_handle<> h) -> bool {
    // TODO: Implementation
    // - Store awaiting coroutine handle
    // - Store current executor
    // - If already notified, return false (resume immediately)
    // - Otherwise return true (suspend)
    return true;
  }

  auto await_resume() const noexcept -> void {
    // Nothing to return
  }
};

inline auto notify_event::wait() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - Create and co_await awaiter
  co_await awaiter{this};
}

inline auto notify_event::notify() -> void {
  // TODO: Implementation
  // - Set notified flag
  // - If coroutine is waiting, post resume to its executor
}

}  // namespace rediscoro::detail
