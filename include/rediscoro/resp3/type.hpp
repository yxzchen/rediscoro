#pragma once

#include <rediscoro/assert.hpp>

#include <ostream>
#include <string>
#include <vector>

namespace rediscoro::resp3 {

/** @brief RESP3 data types.

    The RESP3 specification can be found at
    <a href="https://github.com/redis/redis-specifications/blob/master/protocol/RESP3.md"></a>
 */
enum class type3 {
  // Aggregate
  array,
  push,
  set,
  map,
  attribute,

  /// Simple
  simple_string,
  simple_error,
  number,
  doublean,
  boolean,
  big_number,
  null,
  blob_error,
  verbatim_string,
  blob_string,
  streamed_string,
  streamed_string_part,

  invalid,
};

/** @brief Converts the data type to a string.
 *
 *  @relates type
 *  @param t The type to convert.
 */
auto to_string(type3 type) noexcept -> char const*;

/** @brief Writes the type to the output stream.
 *
 *  @relates type
 *  @param os Output stream.
 *  @param t The type to stream.
 */
auto operator<<(std::ostream& os, type3 type) -> std::ostream&;

/** @brief Checks whether the data type is an aggregate.
 *
 *  @relates type
 *  @param t The type to check.
 *  @returns True if the given type is an aggregate.
 */
constexpr auto is_aggregate(type3 type) noexcept -> bool {
  switch (type) {
    case type3::array:
    case type3::push:
    case type3::set:
    case type3::map:
    case type3::attribute:
      return true;

    default:
      return false;
  }
}

/** @brief Checks whether the data type is an error.
 *
 *  @relates type
 *  @param t The type to check.
 *  @returns True if the given type is an error type.
 */
constexpr auto is_error(type3 type) noexcept -> bool {
  return type == type3::simple_error || type == type3::blob_error;
}

/** @brief Checks whether the data type is array-like (array, set, or push).
 *
 *  @relates type
 *  @param t The type to check.
 *  @returns True if the given type is array-like.
 */
constexpr auto is_array_like(type3 type) noexcept -> bool {
  return type == type3::array || type == type3::set || type == type3::push;
}

/** @brief Checks whether the data type is map-like (map or attribute).
 *
 *  @relates type
 *  @param t The type to check.
 *  @returns True if the given type is map-like.
 */
constexpr auto is_map_like(type3 type) noexcept -> bool {
  return type == type3::map || type == type3::attribute;
}

// For map and attribute data types this function returns 2.  All
// other types have value 1.
constexpr auto element_multiplicity(type3 type) noexcept -> std::size_t {
  switch (type) {
    case type3::map:
    case type3::attribute:
      return 2ULL;

    default:
      return 1ULL;
  }
}

// Returns the wire code of a given type.
constexpr auto to_code(type3 type) noexcept -> char {
  switch (type) {
      // clang-format off
    case type3::blob_error:           return '!';
    case type3::verbatim_string:      return '=';
    case type3::blob_string:          return '$';
    case type3::streamed_string_part: return ';';
    case type3::simple_error:         return '-';
    case type3::number:               return ':';
    case type3::doublean:             return ',';
    case type3::boolean:              return '#';
    case type3::big_number:           return '(';
    case type3::simple_string:        return '+';
    case type3::null:                 return '_';
    case type3::push:                 return '>';
    case type3::set:                  return '~';
    case type3::array:                return '*';
    case type3::attribute:            return '|';
    case type3::map:                  return '%';

     default: REDISCORO_ASSERT(false); return ' ';
      // clang-format on
  }
}

// Converts a wire-format RESP3 type (char) to a resp3 type.
constexpr auto to_type(char c) noexcept -> type3 {
  switch (c) {
      // clang-format off
    case '!': return type3::blob_error;
    case '=': return type3::verbatim_string;
    case '$': return type3::blob_string;
    case ';': return type3::streamed_string_part;
    case '-': return type3::simple_error;
    case ':': return type3::number;
    case ',': return type3::doublean;
    case '#': return type3::boolean;
    case '(': return type3::big_number;
    case '+': return type3::simple_string;
    case '_': return type3::null;
    case '>': return type3::push;
    case '~': return type3::set;
    case '*': return type3::array;
    case '|': return type3::attribute;
    case '%': return type3::map;
    default:  return type3::invalid;
      // clang-format on
  }
}

}  // namespace rediscoro::resp3
