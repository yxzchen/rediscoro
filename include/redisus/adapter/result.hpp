#pragma once

#include <redisus/error.hpp>
#include <redisus/expected.hpp>
#include <redisus/resp3/type.hpp>

#include <string>

namespace redisus::adapter {

struct error {
  resp3::type3 data_type = resp3::type3::invalid;

  std::string diagnostic;
};

template <typename T>
using result = expected<T, error>;

}  // namespace redisus::adapter
