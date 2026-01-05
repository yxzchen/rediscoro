#pragma once

#include <system_error>
#include <type_traits>

namespace rediscoro::resp3 {

enum class error {
  // Data incomplete, need more input (not a real protocol error)
  needs_more = 1,

  // Protocol errors
  invalid_type_byte,

  // Generic format error
  invalid_null,
  invalid_boolean,
  invalid_bulk_trailer,
  invalid_double,
  invalid_integer,
  invalid_length,
  invalid_map_pairs,

  invalid_state,
  tree_not_consumed,

  // Parser is in failed state (prior protocol error)
  parser_failed,
};

auto make_error_code(error e) -> std::error_code;

}  // namespace rediscoro::resp3

namespace std {

template <>
struct is_error_code_enum<rediscoro::resp3::error> : std::true_type {};

}  // namespace std

#include <rediscoro/resp3/impl/error.ipp>
