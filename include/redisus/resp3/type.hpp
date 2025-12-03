/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <redisus/assert.hpp>

#include <ostream>
#include <string>
#include <vector>

namespace redisus::resp3 {

/** @brief RESP3 data types.

    The RESP3 specification can be found at
    <a href="https://github.com/redis/redis-specifications/blob/master/protocol/RESP3.md"></a>
 */
enum class type_t {  /// Aggregate
  array,
  /// Aaggregate
  push,
  /// Aggregate
  set,
  /// Aggregate
  map,
  /// Aggregate
  attribute,
  /// Simple
  simple_string,
  /// Simple
  simple_error,
  /// Simple
  number,
  /// Simple
  doublean,
  /// Simple
  boolean,
  /// Simple
  big_number,
  /// Simple
  null,
  /// Simple
  blob_error,
  /// Simple
  verbatim_string,
  /// Simple
  blob_string,
  /// Simple
  streamed_string,
  /// Simple
  streamed_string_part,
  /// Invalid
  invalid
};

/** @brief Converts the data type to a string.
 *
 *  @relates type
 *  @param t The type to convert.
 */
auto to_string(type_t type) noexcept -> char const*;

/** @brief Writes the type to the output stream.
 *
 *  @relates type
 *  @param os Output stream.
 *  @param t The type to stream.
 */
auto operator<<(std::ostream& os, type_t type) -> std::ostream&;

/** @brief Checks whether the data type is an aggregate.
 *
 *  @relates type
 *  @param t The type to check.
 *  @returns True if the given type is an aggregate.
 */
constexpr auto is_aggregate(type_t type) noexcept -> bool {
  switch (type) {
    case type_t::array:
    case type_t::push:
    case type_t::set:
    case type_t::map:
    case type_t::attribute:
      return true;

    default:
      return false;
  }
}

// For map and attribute data types this function returns 2.  All
// other types have value 1.
constexpr auto element_multiplicity(type_t type) noexcept -> std::size_t {
  switch (type) {
    case type_t::map:
    case type_t::attribute:
      return 2ULL;

    default:
      return 1ULL;
  }
}

// Returns the wire code of a given type.
constexpr auto to_code(type_t type) noexcept -> char {
  switch (type) {
      // clang-format off
    case type_t::blob_error:           return '!';
    case type_t::verbatim_string:      return '=';
    case type_t::blob_string:          return '$';
    case type_t::streamed_string_part: return ';';
    case type_t::simple_error:         return '-';
    case type_t::number:               return ':';
    case type_t::doublean:             return ',';
    case type_t::boolean:              return '#';
    case type_t::big_number:           return '(';
    case type_t::simple_string:        return '+';
    case type_t::null:                 return '_';
    case type_t::push:                 return '>';
    case type_t::set:                  return '~';
    case type_t::array:                return '*';
    case type_t::attribute:            return '|';
    case type_t::map:                  return '%';

     default: REDISUS_ASSERT(false); return ' ';
      // clang-format on
  }
}

// Converts a wire-format RESP3 type (char) to a resp3 type.
constexpr auto to_type(char c) noexcept -> type_t {
  switch (c) {
      // clang-format off
    case '!': return type_t::blob_error;
    case '=': return type_t::verbatim_string;
    case '$': return type_t::blob_string;
    case ';': return type_t::streamed_string_part;
    case '-': return type_t::simple_error;
    case ':': return type_t::number;
    case ',': return type_t::doublean;
    case '#': return type_t::boolean;
    case '(': return type_t::big_number;
    case '+': return type_t::simple_string;
    case '_': return type_t::null;
    case '>': return type_t::push;
    case '~': return type_t::set;
    case '*': return type_t::array;
    case '|': return type_t::attribute;
    case '%': return type_t::map;
    default:  return type_t::invalid;
      // clang-format on
  }
}

}  // namespace redisus::resp3
