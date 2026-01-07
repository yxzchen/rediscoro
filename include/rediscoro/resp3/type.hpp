#pragma once

#include <optional>
#include <string_view>

namespace rediscoro::resp3 {

// clang-format off

/// RESP3 protocol data types
enum class type3 {
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
[[nodiscard]] constexpr auto type_to_code(type3 t) noexcept -> char {
  switch (t) {
    case type3::simple_string:   return '+';
    case type3::simple_error:    return '-';
    case type3::integer:         return ':';
    case type3::double_type:     return ',';
    case type3::boolean:         return '#';
    case type3::big_number:      return '(';
    case type3::null:            return '_';
    case type3::bulk_string:     return '$';
    case type3::bulk_error:      return '!';
    case type3::verbatim_string: return '=';
    case type3::array:           return '*';
    case type3::map:             return '%';
    case type3::set:             return '~';
    case type3::attribute:       return '|';
    case type3::push:            return '>';
  }
  return '\0';
}

/// Convert RESP3 leading prefix byte to a type.
[[nodiscard]] constexpr auto code_to_type(char b) noexcept -> std::optional<type3> {
  switch (b) {
    case '+': return type3::simple_string;
    case '-': return type3::simple_error;
    case ':': return type3::integer;
    case ',': return type3::double_type;
    case '#': return type3::boolean;
    case '(': return type3::big_number;
    case '_': return type3::null;
    case '$': return type3::bulk_string;
    case '!': return type3::bulk_error;
    case '=': return type3::verbatim_string;
    case '*': return type3::array;
    case '%': return type3::map;
    case '~': return type3::set;
    case '|': return type3::attribute;
    case '>': return type3::push;
    default:  return std::nullopt;
  }
}

/// User-readable type name (for diagnostics/logging).
[[nodiscard]] constexpr auto type_name(type3 t) noexcept -> std::string_view {
  switch (t) {
    case type3::simple_string:   return "simple_string";
    case type3::simple_error:    return "simple_error";
    case type3::integer:         return "integer";
    case type3::double_type:     return "double";
    case type3::boolean:         return "boolean";
    case type3::big_number:      return "big_number";
    case type3::null:            return "null";
    case type3::bulk_string:     return "bulk_string";
    case type3::bulk_error:      return "bulk_error";
    case type3::verbatim_string: return "verbatim_string";
    case type3::array:           return "array";
    case type3::map:             return "map";
    case type3::set:             return "set";
    case type3::attribute:       return "attribute";
    case type3::push:            return "push";
  }
  return "<unknown>";
}

}  // namespace rediscoro::resp3
