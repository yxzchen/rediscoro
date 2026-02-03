#pragma once

#include <optional>
#include <string_view>

namespace rediscoro::resp3 {

// clang-format off

/// RESP3 protocol data kinds.
enum class kind {
  // Simple
  simple_string,   // +
  simple_error,    // -
  integer,         // :
  double_number,   // ,
  boolean,         // #
  big_number,      // (
  null,            // _

  // Bulk
  bulk_string,     // $
  bulk_error,      // !
  verbatim_string, // =

  // Aggregate
  array,           // *
  map,             // %
  set,             // ~
  attribute,       // | (metadata for other kinds)
  push,            // >
};

// clang-format on

/// Convert RESP3 kind to its leading prefix byte in the wire format.
[[nodiscard]] constexpr auto kind_to_prefix(kind k) noexcept -> char {
  switch (k) {
    case kind::simple_string:   return '+';
    case kind::simple_error:    return '-';
    case kind::integer:         return ':';
    case kind::double_number:   return ',';
    case kind::boolean:         return '#';
    case kind::big_number:      return '(';
    case kind::null:            return '_';
    case kind::bulk_string:     return '$';
    case kind::bulk_error:      return '!';
    case kind::verbatim_string: return '=';
    case kind::array:           return '*';
    case kind::map:             return '%';
    case kind::set:             return '~';
    case kind::attribute:       return '|';
    case kind::push:            return '>';
  }
  return '\0';
}

/// Convert RESP3 leading prefix byte to a kind.
[[nodiscard]] constexpr auto prefix_to_kind(char b) noexcept -> std::optional<kind> {
  switch (b) {
    case '+': return kind::simple_string;
    case '-': return kind::simple_error;
    case ':': return kind::integer;
    case ',': return kind::double_number;
    case '#': return kind::boolean;
    case '(': return kind::big_number;
    case '_': return kind::null;
    case '$': return kind::bulk_string;
    case '!': return kind::bulk_error;
    case '=': return kind::verbatim_string;
    case '*': return kind::array;
    case '%': return kind::map;
    case '~': return kind::set;
    case '|': return kind::attribute;
    case '>': return kind::push;
    default:  return std::nullopt;
  }
}

/// User-readable kind name (for diagnostics/logging).
[[nodiscard]] constexpr auto kind_name(kind k) noexcept -> std::string_view {
  switch (k) {
    case kind::simple_string:   return "simple_string";
    case kind::simple_error:    return "simple_error";
    case kind::integer:         return "integer";
    case kind::double_number:   return "double";
    case kind::boolean:         return "boolean";
    case kind::big_number:      return "big_number";
    case kind::null:            return "null";
    case kind::bulk_string:     return "bulk_string";
    case kind::bulk_error:      return "bulk_error";
    case kind::verbatim_string: return "verbatim_string";
    case kind::array:           return "array";
    case kind::map:             return "map";
    case kind::set:             return "set";
    case kind::attribute:       return "attribute";
    case kind::push:            return "push";
  }
  return "<unknown>";
}

}  // namespace rediscoro::resp3

