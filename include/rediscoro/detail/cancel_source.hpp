#pragma once

#include <atomic>
#include <utility>

namespace rediscoro::detail {

/// Cancellation source for connection lifecycle.
///
/// Responsibilities:
/// - Thread-safe cancellation request
/// - Lightweight check for cancellation
///
/// Note: This is simpler than iocoro::cancellation_token because
/// we don't need callbacks or hierarchical cancellation.
class cancel_source {
public:
  cancel_source() = default;
  cancel_source(cancel_source const&) = delete;
  auto operator=(cancel_source const&) -> cancel_source& = delete;
  cancel_source(cancel_source&&) = delete;
  auto operator=(cancel_source&&) -> cancel_source& = delete;

  /// Request cancellation.
  auto request_cancel() noexcept -> void {
    cancelled_.store(true, std::memory_order_release);
  }

  /// Check if cancellation was requested.
  [[nodiscard]] auto is_cancelled() const noexcept -> bool {
    return cancelled_.load(std::memory_order_acquire);
  }

  /// Reset cancellation state.
  auto reset() noexcept -> void {
    cancelled_.store(false, std::memory_order_release);
  }

private:
  std::atomic<bool> cancelled_{false};
};

}  // namespace rediscoro::detail
