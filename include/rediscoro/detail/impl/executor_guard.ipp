#pragma once

#include <rediscoro/detail/executor_guard.hpp>

namespace rediscoro::detail {

inline auto executor_guard::get_io_executor() const -> iocoro::io_executor {
  return io_executor_;
}

}  // namespace rediscoro::detail
