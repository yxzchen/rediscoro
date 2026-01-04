#pragma once

#include <rediscoro/resp3/type.hpp>
#include <rediscoro/resp3/value.hpp>
#include <variant>

namespace rediscoro::resp3 {

/// RESP3 node representing any RESP3 value
/// This is the main structure for storing RESP3 data in a tree format
struct node {
  using value_type = std::variant<
    // Simple types
    simple_string,
    simple_error,
    integer,
    double_type,
    boolean,
    big_number,
    null,

    // Bulk types
    bulk_string,
    bulk_error,
    verbatim_string,

    // Aggregate types
    array,
    map,
    set,
    push,
  >;

  value_type value;

  // Default constructor - creates a null node
  node() : value(null{}) {}

  // Constructor from any supported type
  template <typename T>
  explicit node(T&& val) : value(std::forward<T>(val)) {}

  /// Get the type of this node
  [[nodiscard]] auto get_type() const -> type { return static_cast<type>(value.index()); }

  /// Check if this node is of a specific type
  template <typename T>
  [[nodiscard]] auto is() const -> bool {
    return std::holds_alternative<T>(value);
  }

  /// Get the value as a specific type
  /// Throws std::bad_variant_access if the type doesn't match
  template <typename T>
  [[nodiscard]] auto as() -> T& {
    return std::get<T>(value);
  }

  template <typename T>
  [[nodiscard]] auto as() const -> const T& {
    return std::get<T>(value);
  }

  /// Try to get the value as a specific type
  /// Returns nullptr if the type doesn't match
  template <typename T>
  [[nodiscard]] auto try_as() -> T* {
    return std::get_if<T>(&value);
  }

  template <typename T>
  [[nodiscard]] auto try_as() const -> const T* {
    return std::get_if<T>(&value);
  }

  /// Check if this is a null node
  [[nodiscard]] auto is_null() const -> bool { return is<null>(); }

  /// Check if this is an aggregate type (array, map, set, push)
  [[nodiscard]] auto is_aggregate() const -> bool {
    return is<array>() || is<map>() || is<set>() || is<push>();
  }

  /// Check if this is a simple type
  [[nodiscard]] auto is_simple() const -> bool {
    return is<simple_string>() || is<simple_error>() || is<integer>() || is<double_type>() ||
           is<boolean>() || is<big_number>() || is<null>();
  }

  /// Check if this is a bulk type
  [[nodiscard]] auto is_bulk() const -> bool {
    return is<bulk_string>() || is<bulk_error>() || is<verbatim_string>();
  }

  /// Check if this is an error type (simple_error or bulk_error)
  [[nodiscard]] auto is_error() const -> bool { return is<simple_error>() || is<bulk_error>(); }

  /// Check if this is a string type (simple_string, bulk_string, or verbatim_string)
  [[nodiscard]] auto is_string() const -> bool {
    return is<simple_string>() || is<bulk_string>() || is<verbatim_string>();
  }
};

}  // namespace rediscoro::resp3
