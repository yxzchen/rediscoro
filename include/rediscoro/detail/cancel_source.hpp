#pragma once

#include <atomic>
#include <memory>

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
  cancel_source() : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}

  /// Request cancellation.
  auto request_cancel() noexcept -> void {
    cancelled_->store(true, std::memory_order_release);
  }

  /// Check if cancellation was requested.
  [[nodiscard]] auto is_cancelled() const noexcept -> bool {
    return cancelled_->load(std::memory_order_acquire);
  }

  /// Reset cancellation state.
  auto reset() noexcept -> void {
    cancelled_->store(false, std::memory_order_release);
  }

private:
  std::shared_ptr<std::atomic<bool>> cancelled_;
};

}  // namespace rediscoro::detail
