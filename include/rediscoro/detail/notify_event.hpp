#pragma once

#include <iocoro/awaitable.hpp>
#include <iocoro/any_executor.hpp>
#include <iocoro/io_executor.hpp>

#include <atomic>
#include <coroutine>
#include <memory>
#include <mutex>
#include <optional>

namespace rediscoro::detail {

/// Executor-aware coroutine notification primitive.
///
/// Signal Semantics (CRITICAL):
/// - This is a COUNTING notification primitive
/// - Multiple notify() calls increment an internal counter
/// - Each wait() consumes one count
/// - If counter > 0 when wait() is called, returns immediately
/// - If counter == 0, suspends and waits for next notify()
///
/// Why counting: Prevents signal loss in connection IO loops
/// - enqueue() + enqueue() + notify() + notify() = 2 wakeups guaranteed
/// - Without counting, the second notify() could be lost
///
/// Responsibilities:
/// - Count-based notification (no signal loss)
/// - Resume awaiting coroutine on its original executor
/// - Thread-safe notify from any thread
///
/// Constraints:
/// - wait() can be called multiple times (each consumes one count)
/// - notify() can be called from any thread
/// - Resume happens on the executor bound to the awaiting coroutine
/// - INVARIANT: A notify() ALWAYS results in either immediate resume or future resume
///
/// Thread Safety (CRITICAL IMPLEMENTATION DETAIL):
/// - notify() can be called from any thread
/// - wait() must be called from a coroutine context
/// - Internal state protected by mutex
///
/// Race Condition Prevention (MUST IMPLEMENT CORRECTLY):
/// The following sequence MUST be atomic:
///   1. Check count_
///   2. Decide to suspend
///   3. Register coroutine_handle
///   4. Actually suspend
///
/// MUST-NOT-VIOLATE Implementation Rule (locks in the atomicity boundary):
/// - wait() MUST "consume-or-register" as one atomic decision:
///   - either it consumes one count and returns without suspending, OR
///   - it registers the waiter (handle + executor) and suspends
/// - The count check, waiter registration, and suspend decision MUST NOT be separated.
///
/// Why this is mandatory:
/// A common-but-wrong pattern is:
///   if (count_ > 0) { --count_; return; }
///   register_waiter();
///   suspend();
/// which introduces a window where notify() increments count_ but the waiter is not
/// registered yet, causing a lost signal and a deadlock.
///
/// WRONG implementation (has race):
///   if (count_ == 0) {           // Check
///     awaiting_ = handle;        // Register
///     // RACE WINDOW: notify() could increment count here
///     return true; // suspend    // Suspend decision
///   }
///   // notify() post-resume might be lost!
///
/// CORRECT implementation pattern:
///   std::lock_guard lock(mutex_);
///   if (count_.load() > 0) {
///     count_.fetch_sub(1);
///     return false; // don't suspend
///   }
///   awaiting_ = handle;
///   executor_ = current_executor;
///   return true; // suspend
///   // notify() will see awaiting_ is set
///
/// This ensures: check + register + suspend decision is atomic
/// Therefore: notify() either sees count or sees awaiting coroutine
class notify_event {
public:
  notify_event() = default;
  ~notify_event() = default;

  notify_event(notify_event const&) = delete;
  auto operator=(notify_event const&) -> notify_event& = delete;

  notify_event(notify_event&&) = delete;
  auto operator=(notify_event&&) -> notify_event& = delete;

  /// Wait for notification.
  /// Can be called multiple times (each call consumes one count).
  auto wait() -> iocoro::awaitable<void>;

  /// Signal the waiting coroutine.
  /// Can be called from any thread.
  auto notify() -> void;

  /// Check if there are pending notifications (for optimization).
  [[nodiscard]] auto is_ready() const noexcept -> bool {
    return count_.load(std::memory_order_acquire) > 0;
  }

private:
  struct awaiter;

  // Notification count (atomic for lock-free check)
  std::atomic<std::size_t> count_{0};

  // Mutex protects awaiting coroutine state
  std::mutex mutex_{};
  std::optional<std::coroutine_handle<>> awaiting_{};
  iocoro::any_executor executor_{};
};

}  // namespace rediscoro::detail

#include <rediscoro/impl/notify_event.ipp>
