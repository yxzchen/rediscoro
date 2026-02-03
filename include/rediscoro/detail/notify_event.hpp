#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/condition_event.hpp>

namespace rediscoro::detail {

/// A thin wrapper around `iocoro::condition_event` with a minimal API.
///
/// Semantics:
/// - Counting (ticketed) wakeups (no lost signals).
/// - Stop-aware: waiting will resume with operation_aborted when stop is requested.
class notify_event {
 public:
  notify_event() = default;
  ~notify_event() = default;

  notify_event(notify_event const&) = delete;
  auto operator=(notify_event const&) -> notify_event& = delete;

  notify_event(notify_event&&) = delete;
  auto operator=(notify_event&&) -> notify_event& = delete;

  /// Wait for notification.
  auto wait() -> iocoro::awaitable<void> {
    (void)co_await ev_.async_wait();
    co_return;
  }

  /// Signal the waiting coroutine.
  void notify() noexcept { ev_.notify(); }

 private:
  iocoro::condition_event ev_{};
};

}  // namespace rediscoro::detail
