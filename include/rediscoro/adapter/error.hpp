#pragma once

#include <system_error>
#include <type_traits>

namespace rediscoro::adapter {

enum class error {
  type_mismatch = 1,
  unexpected_null,
  value_out_of_range,
  size_mismatch,
  invalid_value,
};

auto make_error_code(error e) -> std::error_code;

}  // namespace rediscoro::adapter

namespace std {

template <>
struct is_error_code_enum<rediscoro::adapter::error> : std::true_type {};

}  // namespace std

#include <rediscoro/adapter/impl/error.ipp>
