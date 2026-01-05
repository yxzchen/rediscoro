#pragma once

#include <rediscoro/resp3/message.hpp>
#include <rediscoro/resp3/type.hpp>
#include <rediscoro/expected.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace rediscoro::resp3 {

enum class adapter_error_kind : std::uint8_t {
  type_mismatch,
  unexpected_null,
  value_out_of_range,
  size_mismatch,
  invalid_value,
};

struct adapter_path_index {
  std::size_t index{};
};

struct adapter_path_key {
  std::string key;  // owning for stable diagnostics
};

struct adapter_path_field {
  std::string field;  // owning for stable diagnostics
};

using adapter_path_element = std::variant<adapter_path_index, adapter_path_key, adapter_path_field>;

struct adapter_error {
  adapter_error_kind kind{};
  std::optional<type3> expected_type{};
  std::optional<type3> actual_type{};
  std::vector<adapter_path_element> path{};
  std::string message{};

  auto with_path(adapter_path_element el) && -> adapter_error {
    path.push_back(std::move(el));
    return std::move(*this);
  }
};

namespace detail {

template <typename...>
inline constexpr bool dependent_false_v = false;

template <typename T>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename T>
struct is_std_optional : std::false_type {};
template <typename U>
struct is_std_optional<std::optional<U>> : std::true_type {};

template <typename T>
inline constexpr bool is_std_optional_v = is_std_optional<remove_cvref_t<T>>::value;

template <typename T>
struct optional_value_type;
template <typename U>
struct optional_value_type<std::optional<U>> { using type = U; };

template <typename T>
using optional_value_type_t = typename optional_value_type<remove_cvref_t<T>>::type;

template <typename T>
concept string_like =
  std::is_same_v<remove_cvref_t<T>, std::string> ||
  std::is_same_v<remove_cvref_t<T>, std::string_view>;

template <typename T>
concept bool_like = std::is_same_v<remove_cvref_t<T>, bool>;

template <typename T>
concept integral_like =
  std::is_integral_v<remove_cvref_t<T>> && !bool_like<T>;

template <typename T>
concept double_like =
  std::is_floating_point_v<remove_cvref_t<T>>;

template <typename T>
concept has_value_type = requires { typename remove_cvref_t<T>::value_type; };

template <typename T>
concept sequence_like =
  has_value_type<T> &&
  requires(remove_cvref_t<T>& c, typename remove_cvref_t<T>::value_type v) {
    c.push_back(std::move(v));
  };

template <typename T>
concept map_like =
  requires {
    typename remove_cvref_t<T>::key_type;
    typename remove_cvref_t<T>::mapped_type;
  } &&
  requires(remove_cvref_t<T>& m, typename remove_cvref_t<T>::key_type k, typename remove_cvref_t<T>::mapped_type v) {
    m.emplace(std::move(k), std::move(v));
  };

struct ignore_t {};
inline constexpr ignore_t ignore{};

inline auto make_type_mismatch(std::optional<type3> expected, type3 actual, std::string msg = {}) -> adapter_error {
  return adapter_error{
    .kind = adapter_error_kind::type_mismatch,
    .expected_type = expected,
    .actual_type = actual,
    .path = {},
    .message = std::move(msg),
  };
}

inline auto make_unexpected_null(type3 actual) -> adapter_error {
  return adapter_error{
    .kind = adapter_error_kind::unexpected_null,
    .expected_type = std::nullopt,
    .actual_type = actual,
    .path = {},
    .message = "unexpected null",
  };
}

}  // namespace detail

template <typename T>
auto adapt(const message& msg) -> rediscoro::expected<T, adapter_error>;

namespace detail {

template <typename T>
auto adapt_scalar(const message& msg) -> rediscoro::expected<T, adapter_error> {
  using U = remove_cvref_t<T>;

  if constexpr (string_like<U>) {
    if (msg.is<simple_string>()) {
      if constexpr (std::is_same_v<U, std::string_view>) {
        return std::string_view(msg.as<simple_string>().data);
      } else {
        return msg.as<simple_string>().data;
      }
    }
    if (msg.is<bulk_string>()) {
      if constexpr (std::is_same_v<U, std::string_view>) {
        return std::string_view(msg.as<bulk_string>().data);
      } else {
        return msg.as<bulk_string>().data;
      }
    }
    if (msg.is<verbatim_string>()) {
      if constexpr (std::is_same_v<U, std::string_view>) {
        return std::string_view(msg.as<verbatim_string>().data);
      } else {
        return msg.as<verbatim_string>().data;
      }
    }
    return rediscoro::unexpected(make_type_mismatch(std::nullopt, msg.get_type(), "expected string-like"));
  } else if constexpr (integral_like<U>) {
    if (!msg.is<integer>()) {
      return rediscoro::unexpected(make_type_mismatch(type3::integer, msg.get_type(), "expected integer"));
    }
    auto v = msg.as<integer>().value;
    if constexpr (sizeof(U) < sizeof(std::int64_t)) {
      if (v < static_cast<std::int64_t>((std::numeric_limits<U>::min)()) ||
          v > static_cast<std::int64_t>((std::numeric_limits<U>::max)())) {
        return rediscoro::unexpected(adapter_error{
          .kind = adapter_error_kind::value_out_of_range,
          .expected_type = type3::integer,
          .actual_type = msg.get_type(),
          .path = {},
          .message = "integer out of range",
        });
      }
    }
    return static_cast<U>(v);
  } else if constexpr (bool_like<U>) {
    if (!msg.is<boolean>()) {
      return rediscoro::unexpected(make_type_mismatch(type3::boolean, msg.get_type(), "expected boolean"));
    }
    return msg.as<boolean>().value;
  } else if constexpr (double_like<U>) {
    if (!msg.is<double_type>()) {
      return rediscoro::unexpected(make_type_mismatch(type3::double_type, msg.get_type(), "expected double"));
    }
    return static_cast<U>(msg.as<double_type>().value);
  } else {
    static_assert(dependent_false_v<U>, "no scalar adapter for this type");
  }
}

template <typename T>
auto adapt_optional(const message& msg) -> rediscoro::expected<T, adapter_error> {
  using V = optional_value_type_t<T>;
  if (msg.is<null>()) {
    return T{std::nullopt};
  }
  auto inner = adapt<V>(msg);
  if (!inner) {
    return rediscoro::unexpected(std::move(inner.error()));
  }
  return T{std::move(*inner)};
}

template <typename T>
auto adapt_sequence(const message& msg) -> rediscoro::expected<T, adapter_error> {
  using U = remove_cvref_t<T>;
  using V = typename U::value_type;

  const std::vector<message>* elems = nullptr;
  if (msg.is<array>()) {
    elems = &msg.as<array>().elements;
  } else if (msg.is<set>()) {
    elems = &msg.as<set>().elements;
  } else if (msg.is<push>()) {
    elems = &msg.as<push>().elements;
  } else {
    return rediscoro::unexpected(make_type_mismatch(std::nullopt, msg.get_type(), "expected array/set/push"));
  }

  U out{};
  for (std::size_t i = 0; i < elems->size(); ++i) {
    auto r = adapt<V>((*elems)[i]);
    if (!r) {
      return rediscoro::unexpected(std::move(r.error()).with_path(adapter_path_index{i}));
    }
    out.push_back(std::move(*r));
  }
  return out;
}

template <typename T>
auto adapt_map(const message& msg) -> rediscoro::expected<T, adapter_error> {
  using U = remove_cvref_t<T>;
  using K = typename U::key_type;
  using V = typename U::mapped_type;

  if (!msg.is<map>()) {
    return rediscoro::unexpected(make_type_mismatch(type3::map, msg.get_type(), "expected map"));
  }

  U out{};
  const auto& entries = msg.as<map>().entries;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& [km, vm] = entries[i];
    auto rk = adapt<K>(km);
    if (!rk) {
      return rediscoro::unexpected(std::move(rk.error()).with_path(adapter_path_index{i}).with_path(adapter_path_field{"key"}));
    }
    auto rv = adapt<V>(vm);
    if (!rv) {
      return rediscoro::unexpected(std::move(rv.error()).with_path(adapter_path_index{i}).with_path(adapter_path_field{"value"}));
    }
    out.emplace(std::move(*rk), std::move(*rv));
  }
  return out;
}

template <typename T>
auto adapt_ignore(const message&) -> rediscoro::expected<T, adapter_error> {
  return T{};
}

}  // namespace detail

template <typename T>
auto adapt(const message& msg) -> rediscoro::expected<T, adapter_error> {
  using U = detail::remove_cvref_t<T>;

  if constexpr (std::is_same_v<U, detail::ignore_t>) {
    return detail::adapt_ignore<U>(msg);
  } else if constexpr (detail::is_std_optional_v<U>) {
    return detail::adapt_optional<U>(msg);
  } else if constexpr (detail::sequence_like<U> && !detail::string_like<U>) {
    return detail::adapt_sequence<U>(msg);
  } else if constexpr (detail::map_like<U>) {
    return detail::adapt_map<U>(msg);
  } else {
    return detail::adapt_scalar<U>(msg);
  }
}

}  // namespace rediscoro::resp3


