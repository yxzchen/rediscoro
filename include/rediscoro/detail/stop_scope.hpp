#pragma once

#include <rediscoro/detail/internal_header_access.hpp>

#include <stop_token>

namespace rediscoro::detail {

/// A resettable stop/cancellation scope.
///
/// `std::stop_source` itself cannot be reset; this wrapper provides a convenient `reset()`
/// that swaps in a new source.
class stop_scope {
 public:
  stop_scope() = default;

  [[nodiscard]] auto get_token() const noexcept -> std::stop_token { return src_.get_token(); }

  auto request_stop() noexcept -> void { src_.request_stop(); }

  auto reset() -> void { src_ = std::stop_source{}; }

 private:
  std::stop_source src_{};
};

}  // namespace rediscoro::detail
