#pragma once

#include <rediscoro/resp3/kind.hpp>

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace rediscoro::resp3 {

// Forward declaration
struct message;

/// Simple string value (+)
struct simple_string {
  static constexpr kind kind_id = kind::simple_string;
  std::string_view data;
};

/// Simple error value (-)
struct simple_error {
  static constexpr kind kind_id = kind::simple_error;
  std::string_view message;
};

/// Integer value (:)
struct integer {
  static constexpr kind kind_id = kind::integer;
  std::int64_t value;
};

/// Double value (,)
struct double_number {
  static constexpr kind kind_id = kind::double_number;
  double value;
};

/// Boolean value (#)
struct boolean {
  static constexpr kind kind_id = kind::boolean;
  bool value;
};

/// Big number value (()
struct big_number {
  static constexpr kind kind_id = kind::big_number;
  std::string_view value;  // Stored as string-view to handle arbitrary precision
};

/// Null value (_)
struct null {
  static constexpr kind kind_id = kind::null;
};

/// Bulk string value ($)
struct bulk_string {
  static constexpr kind kind_id = kind::bulk_string;
  std::string_view data;
};

/// Bulk error value (!)
struct bulk_error {
  static constexpr kind kind_id = kind::bulk_error;
  std::string_view message;
};

/// Verbatim string value (=)
struct verbatim_string {
  static constexpr kind kind_id = kind::verbatim_string;
  std::string_view encoding;  // 3-byte encoding type
  std::string_view data;
};

/// Array value (*)
struct array {
  static constexpr kind kind_id = kind::array;
  std::vector<message> elements;
};

/// Map value (%)
/// Stored as vector of key-value pairs to preserve order
struct map {
  static constexpr kind kind_id = kind::map;
  std::vector<std::pair<message, message>> entries;
};

/// Set value (~)
struct set {
  static constexpr kind kind_id = kind::set;
  std::vector<message> elements;
};

/// Attribute value (|)
/// Attributes are metadata that can be attached to any RESP3 value
struct attribute {
  static constexpr kind kind_id = kind::attribute;
  std::vector<std::pair<message, message>> entries;
};

/// Push value (>)
struct push {
  static constexpr kind kind_id = kind::push;
  std::vector<message> elements;
};

}  // namespace rediscoro::resp3
