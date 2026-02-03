#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace rediscoro::adapter::detail {

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
inline constexpr bool is_std_string_v = std::is_same_v<remove_cvref_t<T>, std::string>;

template <typename T>
inline constexpr bool is_std_string_view_v = std::is_same_v<remove_cvref_t<T>, std::string_view>;

template <typename T>
concept bool_like = std::is_same_v<remove_cvref_t<T>, bool>;

template <typename T>
concept integral_like = std::is_integral_v<remove_cvref_t<T>> && !bool_like<T>;

template <typename T>
concept double_like = std::is_floating_point_v<remove_cvref_t<T>>;

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
  requires(remove_cvref_t<T>& m,
           typename remove_cvref_t<T>::key_type k,
           typename remove_cvref_t<T>::mapped_type v) {
    m.emplace(std::move(k), std::move(v));
  };

template <typename T>
struct is_std_array : std::false_type {};
template <typename U, std::size_t N>
struct is_std_array<std::array<U, N>> : std::true_type {};

template <typename T>
inline constexpr bool is_std_array_v = is_std_array<remove_cvref_t<T>>::value;

}  // namespace rediscoro::adapter::detail

