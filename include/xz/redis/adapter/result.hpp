#pragma once

#include <xz/redis/error.hpp>
#include <xz/redis/expected.hpp>
#include <xz/redis/resp3/type.hpp>

#include <string>

namespace xz::redis::adapter {

struct error {
  resp3::type3 data_type = resp3::type3::invalid;
  std::string msg;
};

template <typename T>
using result = expected<T, error>;

}  // namespace xz::redis::adapter
