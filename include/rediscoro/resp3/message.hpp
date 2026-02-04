#pragma once

#include <rediscoro/resp3/kind.hpp>
#include <rediscoro/resp3/value.hpp>

#include <concepts>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace rediscoro::resp3 {

namespace detail {

// Concept to verify that a type has a static kind_id member
template <typename T>
concept has_kind_id = requires {
  { T::kind_id } -> std::convertible_to<kind>;
};

// Compile-time verification that all value types have kind_id
template <typename... Ts>
constexpr bool all_have_kind_id = (has_kind_id<Ts> && ...);

}  // namespace detail

// clang-format off

/// RESP3 message representing a complete RESP3 value
/// This structure represents a fully parsed RESP3 message with optional attributes
///
/// Note: In the RESP3 protocol, during deserialization, data is parsed line by line
/// (between \r\n delimiters). This `message` structure represents the final, complete
/// parsed result after all lines have been processed, not the intermediate parsing state.
///
/// The parser will maintain its own state machine and intermediate data structures,
/// and construct `message` objects as the final output.
struct message {
  using value_type = std::variant<
    // Simple types
    simple_string,
    simple_error,
    integer,
    double_number,
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
    push
  >;

  // Compile-time check: ensure all variant alternatives have kind_id
  static_assert(detail::all_have_kind_id<
    simple_string, simple_error, integer, double_number, boolean, big_number, null,
    bulk_string, bulk_error, verbatim_string,
    array, map, set, push
  >, "All RESP3 value types must have a static kind_id member");

  // The actual value
  value_type value;

  // Optional attributes that modify this value
  // In RESP3, attributes (|) can appear before any value to provide metadata
  std::optional<attribute> attrs;

  // Default constructor - creates a null message
  message() : value(null{}), attrs(std::nullopt) {}

  // Constructor from any supported type
  template <typename T>
    requires std::constructible_from<value_type, T>
  explicit message(T&& val) : value(std::forward<T>(val)), attrs(std::nullopt) {}

  // Constructor with value and attributes
  template <typename T>
    requires std::constructible_from<value_type, T>
  message(T&& val, attribute&& attributes)
    : value(std::forward<T>(val)), attrs(std::move(attributes)) {}

  /// Get the kind of this message
  /// Uses std::visit to retrieve kind_id from the actual value type
  /// This ensures type safety and doesn't rely on variant index ordering
  [[nodiscard]] auto get_kind() const -> kind {
    return std::visit([](const auto& val) -> kind {
      using T = std::decay_t<decltype(val)>;
      return T::kind_id;
    }, value);
  }

  /// Check if this message is of a specific type
  template <typename T>
  [[nodiscard]] bool is() const {
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

  /// Check if this message has attributes
  [[nodiscard]] bool has_attributes() const {
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

  /// Check if this is a null message
  [[nodiscard]] bool is_null() const {
    return is<null>();
  }

  /// Check if this is an aggregate type (array, map, set, push)
  [[nodiscard]] bool is_aggregate() const {
    return is<array>() || is<map>() || is<set>() || is<push>();
  }

  /// Check if this is a simple type
  [[nodiscard]] bool is_simple() const {
    return is<simple_string>() || is<simple_error>() || is<integer>() ||
           is<double_number>() || is<boolean>() || is<big_number>() || is<null>();
  }

  /// Check if this is a bulk type
  [[nodiscard]] bool is_bulk() const {
    return is<bulk_string>() || is<bulk_error>() || is<verbatim_string>();
  }

  /// Check if this is an error type (simple_error or bulk_error)
  [[nodiscard]] bool is_error() const {
    return is<simple_error>() || is<bulk_error>();
  }

  /// Check if this is a string type (simple_string, bulk_string, or verbatim_string)
  [[nodiscard]] bool is_string() const {
    return is<simple_string>() || is<bulk_string>() || is<verbatim_string>();
  }
};

// clang-format on

}  // namespace rediscoro::resp3
