#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <unordered_set>

namespace rediscoro::resp3 {

// Forward declaration
struct message;

/// Simple string value (+)
struct simple_string {
  std::string data;
};

/// Simple error value (-)
struct simple_error {
  std::string message;
};

/// Integer value (:)
struct integer {
  std::int64_t value;
};

/// Double value (,)
struct double_type {
  double value;
};

/// Boolean value (#)
struct boolean {
  bool value;
};

/// Big number value (()
struct big_number {
  std::string value;  // Stored as string to handle arbitrary precision
};

/// Null value (_)
struct null {};

/// Bulk string value ($)
struct bulk_string {
  std::string data;
};

/// Bulk error value (!)
struct bulk_error {
  std::string message;
};

/// Verbatim string value (=)
struct verbatim_string {
  std::string encoding;  // 3-byte encoding type
  std::string data;
};

/// Array value (*)
struct array {
  std::vector<message> elements;
};

/// Map value (%)
/// Stored as vector of key-value pairs to preserve order
struct map {
  std::vector<std::pair<message, message>> entries;
};

/// Set value (~)
struct set {
  std::vector<message> elements;
};

/// Attribute value (|)
/// Attributes are metadata that can be attached to any RESP3 value
struct attribute {
  std::vector<std::pair<message, message>> entries;
};

/// Push value (>)
struct push {
  std::vector<message> elements;
};

}  // namespace rediscoro::resp3
