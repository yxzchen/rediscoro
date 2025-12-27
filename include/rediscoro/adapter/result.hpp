#pragma once

#include <rediscoro/error.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/resp3/type.hpp>

#include <string>

namespace rediscoro::adapter {

struct error {
  std::string message;
};

template <typename T>
using result = expected<T, error>;

}  // namespace rediscoro::adapter
