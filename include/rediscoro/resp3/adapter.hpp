#pragma once

#include <rediscoro/resp3/message.hpp>
#include <rediscoro/resp3/type.hpp>
#include <rediscoro/expected.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <array>
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

struct adapter_path {
  std::vector<std::variant<struct adapter_path_index, struct adapter_path_key, struct adapter_path_field>> elements{};
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
  type3 actual_type{};
  std::optional<type3> expected_type{};
  std::vector<type3> expected_any_of{};  // non-empty only for type_mismatch with multiple acceptable RESP3 types
  std::vector<adapter_path_element> path{};
  mutable std::optional<std::string> cached_message{};

  auto prepend_path(adapter_path_element el) -> void {
    path.insert(path.begin(), std::move(el));
    cached_message.reset();
  }

  [[nodiscard]] auto to_string() const -> const std::string& {
    if (!cached_message.has_value()) {
      cached_message = format_message(*this);
    }
    return *cached_message;
  }

  [[nodiscard]] static auto format_message(const adapter_error& e) -> std::string;
};

struct ignore_t {};
inline constexpr ignore_t ignore{};

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

template <typename T>
struct is_std_array : std::false_type {};
template <typename U, std::size_t N>
struct is_std_array<std::array<U, N>> : std::true_type {};

template <typename T>
inline constexpr bool is_std_array_v = is_std_array<remove_cvref_t<T>>::value;

inline auto make_type_mismatch(type3 expected, type3 actual, std::vector<type3> any_of = {}) -> adapter_error {
  adapter_error e{
    .kind = adapter_error_kind::type_mismatch,
    .actual_type = actual,
    .expected_type = expected,
    .expected_any_of = std::move(any_of),
    .path = {},
    .cached_message = std::nullopt,
  };
  return e;
}

inline auto make_unexpected_null(type3 expected) -> adapter_error {
  adapter_error e{
    .kind = adapter_error_kind::unexpected_null,
    .actual_type = type3::null,
    .expected_type = expected,
    .expected_any_of = {},
    .path = {},
    .cached_message = std::nullopt,
  };
  return e;
}

inline auto make_value_out_of_range(type3 t) -> adapter_error {
  adapter_error e{
    .kind = adapter_error_kind::value_out_of_range,
    .actual_type = t,
    .expected_type = t,
    .expected_any_of = {},
    .path = {},
    .cached_message = std::nullopt,
  };
  return e;
}

inline auto make_size_mismatch(type3 actual, std::size_t expected, std::size_t got) -> adapter_error {
  adapter_error e{
    .kind = adapter_error_kind::size_mismatch,
    .actual_type = actual,
    .expected_type = actual,  // "container type is correct; size is wrong"
    .expected_any_of = {},
    .path = {},
    .cached_message = std::nullopt,
  };
  e.cached_message = adapter_error::format_message(e) + " (expected " + std::to_string(expected) + ", got " + std::to_string(got) + ")";
  return e;
}

}  // namespace detail

template <typename T>
auto adapt(const message& msg) -> rediscoro::expected<T, adapter_error>;

namespace detail {

template <typename T>
auto adapt_scalar(const message& msg) -> rediscoro::expected<T, adapter_error> {
  using U = remove_cvref_t<T>;

  if constexpr (string_like<U>) {
    if (msg.is<null>()) {
      return rediscoro::unexpected(make_unexpected_null(type3::bulk_string));
    }
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
    return rediscoro::unexpected(make_type_mismatch(
      type3::bulk_string, msg.get_type(), {type3::simple_string, type3::bulk_string, type3::verbatim_string}));
  } else if constexpr (integral_like<U>) {
    if (msg.is<null>()) {
      return rediscoro::unexpected(make_unexpected_null(type3::integer));
    }
    if (!msg.is<integer>()) {
      return rediscoro::unexpected(make_type_mismatch(type3::integer, msg.get_type()));
    }
    auto v = msg.as<integer>().value;
    if constexpr (sizeof(U) < sizeof(std::int64_t)) {
      if (v < static_cast<std::int64_t>((std::numeric_limits<U>::min)()) ||
          v > static_cast<std::int64_t>((std::numeric_limits<U>::max)())) {
        return rediscoro::unexpected(make_value_out_of_range(type3::integer));
      }
    }
    return static_cast<U>(v);
  } else if constexpr (bool_like<U>) {
    if (msg.is<null>()) {
      return rediscoro::unexpected(make_unexpected_null(type3::boolean));
    }
    if (!msg.is<boolean>()) {
      return rediscoro::unexpected(make_type_mismatch(type3::boolean, msg.get_type()));
    }
    return msg.as<boolean>().value;
  } else if constexpr (double_like<U>) {
    if (msg.is<null>()) {
      return rediscoro::unexpected(make_unexpected_null(type3::double_type));
    }
    if (!msg.is<double_type>()) {
      return rediscoro::unexpected(make_type_mismatch(type3::double_type, msg.get_type()));
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

  static_assert(!std::is_same_v<remove_cvref_t<V>, ignore_t>,
    "ignore_t is only allowed as the top-level adaptation target");

  const std::vector<message>* elems = nullptr;
  if (msg.is<array>()) {
    elems = &msg.as<array>().elements;
  } else if (msg.is<set>()) {
    elems = &msg.as<set>().elements;
  } else if (msg.is<push>()) {
    elems = &msg.as<push>().elements;
  } else {
    return rediscoro::unexpected(make_type_mismatch(type3::array, msg.get_type(), {type3::array, type3::set, type3::push}));
  }

  U out{};
  for (std::size_t i = 0; i < elems->size(); ++i) {
    auto r = adapt<V>((*elems)[i]);
    if (!r) {
      auto e = std::move(r.error());
      e.prepend_path(adapter_path_index{i});
      return rediscoro::unexpected(std::move(e));
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

  static_assert(!std::is_same_v<remove_cvref_t<K>, ignore_t>,
    "ignore_t is only allowed as the top-level adaptation target");
  static_assert(!std::is_same_v<remove_cvref_t<V>, ignore_t>,
    "ignore_t is only allowed as the top-level adaptation target");

  if (!msg.is<map>()) {
    return rediscoro::unexpected(make_type_mismatch(type3::map, msg.get_type()));
  }

  U out{};
  const auto& entries = msg.as<map>().entries;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& [km, vm] = entries[i];
    auto rk = adapt<K>(km);
    if (!rk) {
      auto e = std::move(rk.error());
      e.prepend_path(adapter_path_field{"key"});
      e.prepend_path(adapter_path_index{i});
      return rediscoro::unexpected(std::move(e));
    }
    auto rv = adapt<V>(vm);
    if (!rv) {
      auto e = std::move(rv.error());
      e.prepend_path(adapter_path_field{"value"});
      e.prepend_path(adapter_path_index{i});
      return rediscoro::unexpected(std::move(e));
    }
    out.emplace(std::move(*rk), std::move(*rv));
  }
  return out;
}

template <typename T>
auto adapt_ignore(const message&) -> rediscoro::expected<T, adapter_error> {
  return T{};
}

template <typename T>
auto adapt_std_array(const message& msg) -> rediscoro::expected<T, adapter_error> {
  using U = remove_cvref_t<T>;
  using V = typename U::value_type;
  constexpr std::size_t N = std::tuple_size_v<U>;

  if (!msg.is<array>()) {
    return rediscoro::unexpected(make_type_mismatch(type3::array, msg.get_type()));
  }
  const auto& elems = msg.as<array>().elements;
  if (elems.size() != N) {
    return rediscoro::unexpected(make_size_mismatch(msg.get_type(), N, elems.size()));
  }

  U out{};
  for (std::size_t i = 0; i < N; ++i) {
    auto r = adapt<V>(elems[i]);
    if (!r) {
      auto e = std::move(r.error());
      e.prepend_path(adapter_path_index{i});
      return rediscoro::unexpected(std::move(e));
    }
    out[i] = std::move(*r);
  }
  return out;
}

}  // namespace detail

template <typename T>
auto adapt(const message& msg) -> rediscoro::expected<T, adapter_error> {
  using U = detail::remove_cvref_t<T>;

  if constexpr (std::is_same_v<U, ignore_t>) {
    return detail::adapt_ignore<U>(msg);
  } else if constexpr (detail::is_std_optional_v<U>) {
    return detail::adapt_optional<U>(msg);
  } else if constexpr (detail::is_std_array_v<U>) {
    return detail::adapt_std_array<U>(msg);
  } else if constexpr (detail::sequence_like<U> && !detail::string_like<U>) {
    return detail::adapt_sequence<U>(msg);
  } else if constexpr (detail::map_like<U>) {
    return detail::adapt_map<U>(msg);
  } else {
    return detail::adapt_scalar<U>(msg);
  }
}

inline auto adapter_error::format_message(const adapter_error& e) -> std::string {
  auto type_to_string = [](type3 t) -> std::string {
    return std::string(type_name(t));
  };

  auto path_to_string = [](const std::vector<adapter_path_element>& path) -> std::string {
    std::string out = "$";
    for (const auto& el : path) {
      std::visit([&](const auto& v) {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, adapter_path_index>) {
          out += "[" + std::to_string(v.index) + "]";
        } else if constexpr (std::is_same_v<V, adapter_path_key>) {
          out += "[\"" + v.key + "\"]";
        } else if constexpr (std::is_same_v<V, adapter_path_field>) {
          out += "." + v.field;
        }
      }, el);
    }
    return out;
  };

  const auto path = path_to_string(e.path);
  switch (e.kind) {
    case adapter_error_kind::type_mismatch: {
      std::string exp = e.expected_type ? type_to_string(*e.expected_type) : "<?>";  // should not happen
      if (!e.expected_any_of.empty()) {
        exp += " (any of: ";
        for (std::size_t i = 0; i < e.expected_any_of.size(); ++i) {
          if (i != 0) {
            exp += ", ";
          }
          exp += type_to_string(e.expected_any_of[i]);
        }
        exp += ")";
      }
      return path + ": expected " + exp + ", got " + type_to_string(e.actual_type);
    }
    case adapter_error_kind::unexpected_null: {
      const std::string exp = e.expected_type ? type_to_string(*e.expected_type) : "non-null";
      return path + ": unexpected null (expected " + exp + ")";
    }
    case adapter_error_kind::value_out_of_range: {
      const std::string t = e.expected_type ? type_to_string(*e.expected_type) : type_to_string(e.actual_type);
      return path + ": value out of range for " + t;
    }
    case adapter_error_kind::size_mismatch: {
      return path + ": size mismatch";
    }
    case adapter_error_kind::invalid_value: {
      return path + ": invalid value";
    }
  }
  return path + ": adapter error";
}

}  // namespace rediscoro::resp3


