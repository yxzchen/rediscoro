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
  static constexpr type3 type_id = type3::simple_string;
  std::string data;
};

/// Simple error value (-)
struct simple_error {
  static constexpr type3 type_id = type3::simple_error;
  std::string message;
};

/// Integer value (:)
struct integer {
  static constexpr type3 type_id = type3::integer;
  std::int64_t value;
};

/// Double value (,)
struct double_type {
  static constexpr type3 type_id = type3::double_type;
  double value;
};

/// Boolean value (#)
struct boolean {
  static constexpr type3 type_id = type3::boolean;
  bool value;
};

/// Big number value (()
struct big_number {
  static constexpr type3 type_id = type3::big_number;
  std::string value;  // Stored as string to handle arbitrary precision
};

/// Null value (_)
struct null {
  static constexpr type3 type_id = type3::null;
};

/// Bulk string value ($)
struct bulk_string {
  static constexpr type3 type_id = type3::bulk_string;
  std::string data;
};

/// Bulk error value (!)
struct bulk_error {
  static constexpr type3 type_id = type3::bulk_error;
  std::string message;
};

/// Verbatim string value (=)
struct verbatim_string {
  static constexpr type3 type_id = type3::verbatim_string;
  std::string encoding;  // 3-byte encoding type
  std::string data;
};

/// Array value (*)
struct array {
  static constexpr type3 type_id = type3::array;
  std::vector<message> elements;
};

/// Map value (%)
/// Stored as vector of key-value pairs to preserve order
struct map {
  static constexpr type3 type_id = type3::map;
  std::vector<std::pair<message, message>> entries;
};

/// Set value (~)
struct set {
  static constexpr type3 type_id = type3::set;
  std::vector<message> elements;
};

/// Attribute value (|)
/// Attributes are metadata that can be attached to any RESP3 value
struct attribute {
  static constexpr type3 type_id = type3::attribute;
  std::vector<std::pair<message, message>> entries;
};

/// Push value (>)
struct push {
  static constexpr type3 type_id = type3::push;
  std::vector<message> elements;
};

}  // namespace rediscoro::resp3
