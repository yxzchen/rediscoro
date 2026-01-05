#pragma once

#include <rediscoro/resp3/type.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace rediscoro::resp3 {

// Forward declaration
struct message;

/// Simple string value (+)
struct simple_string {
  static constexpr type type_id = type::simple_string;
  std::string data;
};

/// Simple error value (-)
struct simple_error {
  static constexpr type type_id = type::simple_error;
  std::string message;
};

/// Integer value (:)
struct integer {
  static constexpr type type_id = type::integer;
  std::int64_t value;
};

/// Double value (,)
struct double_type {
  static constexpr type type_id = type::double_type;
  double value;
};

/// Boolean value (#)
struct boolean {
  static constexpr type type_id = type::boolean;
  bool value;
};

/// Big number value (()
struct big_number {
  static constexpr type type_id = type::big_number;
  std::string value;  // Stored as string to handle arbitrary precision
};

/// Null value (_)
struct null {
  static constexpr type type_id = type::null;
};

/// Bulk string value ($)
struct bulk_string {
  static constexpr type type_id = type::bulk_string;
  std::string data;
};

/// Bulk error value (!)
struct bulk_error {
  static constexpr type type_id = type::bulk_error;
  std::string message;
};

/// Verbatim string value (=)
struct verbatim_string {
  static constexpr type type_id = type::verbatim_string;
  std::string encoding;  // 3-byte encoding type
  std::string data;
};

/// Array value (*)
struct array {
  static constexpr type type_id = type::array;
  std::vector<message> elements;
};

/// Map value (%)
/// Stored as vector of key-value pairs to preserve order
struct map {
  static constexpr type type_id = type::map;
  std::vector<std::pair<message, message>> entries;
};

/// Set value (~)
struct set {
  static constexpr type type_id = type::set;
  std::vector<message> elements;
};

/// Attribute value (|)
/// Attributes are metadata that can be attached to any RESP3 value
struct attribute {
  static constexpr type type_id = type::attribute;
  std::vector<std::pair<message, message>> entries;
};

/// Push value (>)
struct push {
  static constexpr type type_id = type::push;
  std::vector<message> elements;
};

}  // namespace rediscoro::resp3
