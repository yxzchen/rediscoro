#pragma once

#include <rediscoro/adapter/error.hpp>
#include <rediscoro/ignore.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/resp3/message.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace rediscoro::adapter {

/// CRITICAL CONSTRAINT: No user code execution
///
/// The adapter layer MUST NOT execute user-provided code during adaptation.
/// This ensures that pending_response::deliver() does not inline user logic
/// into the connection strand.
///
/// Forbidden:
/// - Calling user-defined constructors (except trivial/standard types)
/// - Invoking user callbacks or function objects
/// - Triggering user operator overloads (except standard library types)
///
/// Allowed:
/// - Standard library types (std::string, std::vector, etc.)
/// - Trivial types (int, double, bool, etc.)
/// - Aggregate types with standard/trivial members
///
/// Why this matters:
/// - adapt<T>() is called from connection IO loops (connection strand)
/// - If T's constructor has side effects (logging, locks, IO), it runs inline
/// - This breaks "no user code in completions" invariant
///
/// Safe types:
///   adapt<std::string>(msg)      // OK: std::string is standard
///   adapt<int64_t>(msg)          // OK: trivial type
///   adapt<std::vector<int>>(msg) // OK: standard container + trivial element
///
/// Unsafe types (currently not prevented, but should be documented):
///   struct MyType {
///     MyType(std::string s) { log_to_file(s); }  // BAD: side effect in ctor
///   };
///   adapt<MyType>(msg)  // DANGER: log_to_file runs on the connection strand
///
/// Future: Could add concept to restrict T to "safe" types.

/// Concept placeholder for safe response types.
///
/// Future enforcement (not currently checked):
/// - Standard library types (std::string, std::vector, etc.)
/// - Trivial types (int, bool, double, etc.)
/// - Aggregates with safe members only
/// - No user-defined constructors with side effects
///
/// To enable static checking in the future, uncomment and use:
///   template <typename T>
///   concept safe_response_type =
///     std::is_trivial_v<T> ||
///     /* is_standard_library_type<T> */ ||
///     /* is_safe_aggregate<T> */;
///
///   template <safe_response_type T>
///   auto adapt(const resp3::message& msg) -> expected<T, error>;
///
/// This placeholder documents the intent and reserves the design space.

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

}  // namespace detail

template <typename T>
auto adapt(const resp3::message& msg) -> expected<T, error>;

namespace detail {

template <typename T>
auto adapt_scalar(const resp3::message& msg) -> expected<T, error> {
  using U = remove_cvref_t<T>;

  if constexpr (string_like<U>) {
    if (msg.is<resp3::null>()) {
      return unexpected(make_unexpected_null(resp3::type3::bulk_string));
    }
    if (msg.is<resp3::simple_string>()) {
      if constexpr (std::is_same_v<U, std::string_view>) {
        return std::string_view(msg.as<resp3::simple_string>().data);
      } else {
        return msg.as<resp3::simple_string>().data;
      }
    }
    if (msg.is<resp3::bulk_string>()) {
      if constexpr (std::is_same_v<U, std::string_view>) {
        return std::string_view(msg.as<resp3::bulk_string>().data);
      } else {
        return msg.as<resp3::bulk_string>().data;
      }
    }
    if (msg.is<resp3::verbatim_string>()) {
      if constexpr (std::is_same_v<U, std::string_view>) {
        return std::string_view(msg.as<resp3::verbatim_string>().data);
      } else {
        return msg.as<resp3::verbatim_string>().data;
      }
    }
    return unexpected(make_type_mismatch(
      msg.get_type(),
      {resp3::type3::simple_string, resp3::type3::bulk_string, resp3::type3::verbatim_string}));
  } else if constexpr (integral_like<U>) {
    if (msg.is<resp3::null>()) {
      return unexpected(make_unexpected_null(resp3::type3::integer));
    }
    if (!msg.is<resp3::integer>()) {
      return unexpected(make_type_mismatch(msg.get_type(), {resp3::type3::integer}));
    }
    const auto v = msg.as<resp3::integer>().value;
    if (v < static_cast<std::int64_t>((std::numeric_limits<U>::min)()) ||
        v > static_cast<std::int64_t>((std::numeric_limits<U>::max)())) {
      return unexpected(make_value_out_of_range(resp3::type3::integer));
    }
    return static_cast<U>(v);
  } else if constexpr (bool_like<U>) {
    if (msg.is<resp3::null>()) {
      return unexpected(make_unexpected_null(resp3::type3::boolean));
    }
    if (!msg.is<resp3::boolean>()) {
      return unexpected(make_type_mismatch(msg.get_type(), {resp3::type3::boolean}));
    }
    return msg.as<resp3::boolean>().value;
  } else if constexpr (double_like<U>) {
    if (msg.is<resp3::null>()) {
      return unexpected(make_unexpected_null(resp3::type3::double_type));
    }
    if (!msg.is<resp3::double_type>()) {
      return unexpected(make_type_mismatch(msg.get_type(), {resp3::type3::double_type}));
    }
    return static_cast<U>(msg.as<resp3::double_type>().value);
  } else {
    static_assert(dependent_false_v<U>, "no scalar adapter for this type");
  }
}

template <typename T>
auto adapt_optional(const resp3::message& msg) -> expected<T, error> {
  using V = optional_value_type_t<T>;
  if (msg.is<resp3::null>()) {
    return T{std::nullopt};
  }
  auto inner = adapt<V>(msg);
  if (!inner) {
    return unexpected(std::move(inner.error()));
  }
  return T{std::move(*inner)};
}

template <typename T>
auto adapt_sequence(const resp3::message& msg) -> expected<T, error> {
  using U = remove_cvref_t<T>;
  using V = typename U::value_type;

  static_assert(!std::is_same_v<remove_cvref_t<V>, ignore_t>,
    "ignore_t is only allowed as the top-level adaptation target");

  const std::vector<resp3::message>* elems = nullptr;
  if (msg.is<resp3::array>()) {
    elems = &msg.as<resp3::array>().elements;
  } else if (msg.is<resp3::set>()) {
    elems = &msg.as<resp3::set>().elements;
  } else if (msg.is<resp3::push>()) {
    elems = &msg.as<resp3::push>().elements;
  } else {
    return unexpected(make_type_mismatch(
      msg.get_type(), {resp3::type3::array, resp3::type3::set, resp3::type3::push}));
  }

  U out{};
  for (std::size_t i = 0; i < elems->size(); ++i) {
    auto r = adapt<V>((*elems)[i]);
    if (!r) {
      auto e = std::move(r.error());
      e.prepend_path(path_index{i});
      return unexpected(std::move(e));
    }
    out.push_back(std::move(*r));
  }
  return out;
}

template <typename T>
auto adapt_map(const resp3::message& msg) -> expected<T, error> {
  using U = remove_cvref_t<T>;
  using K = typename U::key_type;
  using V = typename U::mapped_type;

  static_assert(!std::is_same_v<remove_cvref_t<K>, ignore_t>,
    "ignore_t is only allowed as the top-level adaptation target");
  static_assert(!std::is_same_v<remove_cvref_t<V>, ignore_t>,
    "ignore_t is only allowed as the top-level adaptation target");

  if (!msg.is<resp3::map>()) {
    return unexpected(make_type_mismatch(msg.get_type(), {resp3::type3::map}));
  }

  U out{};
  const auto& entries = msg.as<resp3::map>().entries;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const auto& [km, vm] = entries[i];
    auto rk = adapt<K>(km);
    if (!rk) {
      auto e = std::move(rk.error());
      e.prepend_path(path_field{"key"});
      e.prepend_path(path_index{i});
      return unexpected(std::move(e));
    }
    auto rv = adapt<V>(vm);
    if (!rv) {
      auto e = std::move(rv.error());
      e.prepend_path(path_field{"value"});
      e.prepend_path(path_index{i});
      return unexpected(std::move(e));
    }
    out.emplace(std::move(*rk), std::move(*rv));
  }
  return out;
}

template <typename T>
auto adapt_ignore(const resp3::message&) -> expected<T, error> {
  return T{};
}

template <typename T>
auto adapt_std_array(const resp3::message& msg) -> expected<T, error> {
  using U = remove_cvref_t<T>;
  using V = typename U::value_type;
  constexpr std::size_t N = std::tuple_size_v<U>;

  if (!msg.is<resp3::array>()) {
    return unexpected(make_type_mismatch(msg.get_type(), {resp3::type3::array}));
  }
  const auto& elems = msg.as<resp3::array>().elements;
  if (elems.size() != N) {
    return unexpected(make_size_mismatch(msg.get_type(), N, elems.size()));
  }

  U out{};
  for (std::size_t i = 0; i < N; ++i) {
    auto r = adapt<V>(elems[i]);
    if (!r) {
      auto e = std::move(r.error());
      e.prepend_path(path_index{i});
      return unexpected(std::move(e));
    }
    out[i] = std::move(*r);
  }
  return out;
}

}  // namespace detail

template <typename T>
auto adapt(const resp3::message& msg) -> expected<T, error> {
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

}  // namespace rediscoro::adapter


