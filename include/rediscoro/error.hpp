#pragma once

#include <system_error>
#include <type_traits>

namespace rediscoro {

enum class error {
  /// Operation cancelled.
  operation_aborted = 1,
};

auto make_error_code(error e) -> std::error_code;

}  // namespace rediscoro

namespace std {

template <>
struct is_error_code_enum<rediscoro::error> : std::true_type {};

}  // namespace std
