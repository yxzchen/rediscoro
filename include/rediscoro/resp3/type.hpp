#pragma once

#include <optional>

namespace rediscoro::resp3 {

// clang-format off

/// RESP3 protocol data types
enum class type {
  // Simple types
  simple_string,   // +
  simple_error,    // -
  integer,         // :
  double_type,     // ,
  boolean,         // #
  big_number,      // (
  null,            // _

  // Bulk types
  bulk_string,     // $
  bulk_error,      // !
  verbatim_string, // =

  // Aggregate types
  array,           // *
  map,             // %
  set,             // ~
  attribute,       // | (metadata for other types)
  push,            // >
};

// clang-format on

/// Convert RESP3 type to its leading prefix byte in the wire format.
[[nodiscard]] constexpr auto type_prefix(type t) noexcept -> char {
  switch (t) {
    case type::simple_string:   return '+';
    case type::simple_error:    return '-';
    case type::integer:         return ':';
    case type::double_type:     return ',';
    case type::boolean:         return '#';
    case type::big_number:      return '(';
    case type::null:            return '_';
    case type::bulk_string:     return '$';
    case type::bulk_error:      return '!';
    case type::verbatim_string: return '=';
    case type::array:           return '*';
    case type::map:             return '%';
    case type::set:             return '~';
    case type::attribute:       return '|';
    case type::push:            return '>';
  }
  return '\0';
}

/// Convert RESP3 leading prefix byte to a type.
[[nodiscard]] constexpr auto type_from_prefix(char b) noexcept -> std::optional<type> {
  switch (b) {
    case '+': return type::simple_string;
    case '-': return type::simple_error;
    case ':': return type::integer;
    case ',': return type::double_type;
    case '#': return type::boolean;
    case '(': return type::big_number;
    case '_': return type::null;
    case '$': return type::bulk_string;
    case '!': return type::bulk_error;
    case '=': return type::verbatim_string;
    case '*': return type::array;
    case '%': return type::map;
    case '~': return type::set;
    case '|': return type::attribute;
    case '>': return type::push;
    default:  return std::nullopt;
  }
}

}  // namespace rediscoro::resp3
