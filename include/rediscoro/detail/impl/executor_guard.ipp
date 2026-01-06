#pragma once

#include <rediscoro/detail/executor_guard.hpp>

namespace rediscoro::detail {

inline auto executor_guard::get_io_executor() const -> iocoro::io_executor {
  // TODO: Implementation
  // - Extract io_executor from the strand
  // - This may require accessing iocoro internals or casting

  // Placeholder implementation
  return iocoro::io_executor{};
}

}  // namespace rediscoro::detail
