#pragma once

#include <rediscoro/resp3/buffer.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/resp3/error.hpp>
#include <rediscoro/expected.hpp>

#include <span>
#include <string_view>
#include <optional>
#include <cstdint>

namespace rediscoro::resp3 {

/// RESP3 protocol parser
/// Parses RESP3 messages from a buffer synchronously
class parser {
public:
  explicit parser(buffer& buf);

  /// Parse next complete message from buffer
  /// Returns:
  /// - expected<message, error_code> on success
  /// - parse_error::incomplete_data if more data needed
  /// - other parse_error on protocol violations
  auto parse() -> expected<message, std::error_code>;

  /// Reset parser state
  auto reset() -> void;

private:
  buffer& buffer_;

  /// Maximum nesting depth for aggregate types
  static constexpr std::size_t max_nesting_depth_ = 512;

  /// Current nesting depth
  std::size_t nesting_depth_ = 0;

  // Helper methods for buffer access

  /// Peek one byte without consuming
  auto peek_byte() -> expected<char, std::error_code>;

  /// Read one byte and consume
  auto read_byte() -> expected<char, std::error_code>;

  /// Read until \r\n delimiter (returns line without \r\n)
  auto read_line() -> expected<std::string_view, std::error_code>;

  /// Read exactly n bytes
  auto read_bytes(std::size_t n) -> expected<std::string_view, std::error_code>;

  /// Consume n bytes from buffer
  auto consume(std::size_t n) -> void;

  // Helper methods for parsing primitives

  /// Parse integer from a line (e.g., "123" -> 123)
  auto parse_integer_from_line(std::string_view line) -> expected<std::int64_t, std::error_code>;

  /// Parse double from a line
  auto parse_double_from_line(std::string_view line) -> expected<double, std::error_code>;

  // Type-specific parsing methods

  auto parse_simple_string() -> expected<simple_string, std::error_code>;
  auto parse_simple_error() -> expected<simple_error, std::error_code>;
  auto parse_integer() -> expected<integer, std::error_code>;
  auto parse_bulk_string() -> expected<bulk_string, std::error_code>;
  auto parse_bulk_error() -> expected<bulk_error, std::error_code>;
  auto parse_array() -> expected<array, std::error_code>;
  auto parse_map() -> expected<map, std::error_code>;
  auto parse_set() -> expected<set, std::error_code>;
  auto parse_attribute() -> expected<attribute, std::error_code>;
  auto parse_push() -> expected<push, std::error_code>;
  auto parse_double() -> expected<double_type, std::error_code>;
  auto parse_boolean() -> expected<boolean, std::error_code>;
  auto parse_big_number() -> expected<big_number, std::error_code>;
  auto parse_null() -> expected<null, std::error_code>;
  auto parse_verbatim_string() -> expected<verbatim_string, std::error_code>;

  /// Parse a message (possibly with attributes prefix)
  auto parse_message_internal() -> expected<message, std::error_code>;

  /// Parse message value based on type byte
  auto parse_value(char type_byte) -> expected<message, std::error_code>;
};

}  // namespace rediscoro::resp3
