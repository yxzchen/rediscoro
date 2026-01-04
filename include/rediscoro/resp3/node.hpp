#pragma once

#include <optional>
#include <rediscoro/resp3/type.hpp>
#include <rediscoro/resp3/value.hpp>
#include <variant>

namespace rediscoro::resp3 {

/// RESP3 node representing a complete RESP3 object
/// This structure represents a fully parsed RESP3 value with optional attributes
///
/// Note: In the RESP3 protocol, during deserialization, data is parsed line by line
/// (between \r\n delimiters). This `node` structure represents the final, complete
/// parsed object after all lines have been processed, not the intermediate parsing state.
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
    attribute,
    push
  >;

  // The actual value
  value_type value;

  // Optional attributes that modify this value
  // In RESP3, attributes (|) can appear before any value to provide metadata
  std::optional<attribute> attrs;

  // Default constructor - creates a null node
  node() : value(null{}), attrs(std::nullopt) {}

  // Constructor from any supported type
  template <typename T>
  explicit node(T&& val) : value(std::forward<T>(val)), attrs(std::nullopt) {}

  // Constructor with value and attributes
  template <typename T>
  node(T&& val, attribute&& attributes)
    : value(std::forward<T>(val)), attrs(std::move(attributes)) {}

  /// Get the type of this node
  [[nodiscard]] auto get_type() const -> type {
    return static_cast<type>(value.index());
  }

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

  /// Check if this node has attributes
  [[nodiscard]] auto has_attributes() const -> bool {
    return attrs.has_value();
  }

  /// Get the attributes (throws if no attributes)
  [[nodiscard]] auto get_attributes() -> attribute& {
    return *attrs;
  }

  [[nodiscard]] auto get_attributes() const -> const attribute& {
    return *attrs;
  }

  /// Try to get the attributes (returns nullptr if no attributes)
  [[nodiscard]] auto try_get_attributes() -> attribute* {
    return attrs.has_value() ? &(*attrs) : nullptr;
  }

  [[nodiscard]] auto try_get_attributes() const -> const attribute* {
    return attrs.has_value() ? &(*attrs) : nullptr;
  }

  /// Check if this is a null node
  [[nodiscard]] auto is_null() const -> bool {
    return is<null>();
  }

  /// Check if this is an aggregate type (array, map, set, push)
  [[nodiscard]] auto is_aggregate() const -> bool {
    return is<array>() || is<map>() || is<set>() || is<push>();
  }

  /// Check if this is a simple type
  [[nodiscard]] auto is_simple() const -> bool {
    return is<simple_string>() || is<simple_error>() || is<integer>() ||
           is<double_type>() || is<boolean>() || is<big_number>() || is<null>();
  }

  /// Check if this is a bulk type
  [[nodiscard]] auto is_bulk() const -> bool {
    return is<bulk_string>() || is<bulk_error>() || is<verbatim_string>();
  }

  /// Check if this is an error type (simple_error or bulk_error)
  [[nodiscard]] auto is_error() const -> bool {
    return is<simple_error>() || is<bulk_error>();
  }

  /// Check if this is a string type (simple_string, bulk_string, or verbatim_string)
  [[nodiscard]] auto is_string() const -> bool {
    return is<simple_string>() || is<bulk_string>() || is<verbatim_string>();
  }
};

}  // namespace rediscoro::resp3
