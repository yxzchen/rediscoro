#pragma once

#include <redisus/error.hpp>
#include <redisus/resp3/node.hpp>

#include <charconv>
#include <optional>
#include <type_traits>

namespace redisus::adapter::detail {

// clang-format off
template <class T> struct is_integral_number : std::is_integral<T> { };
template <> struct is_integral_number<bool> : std::false_type { };
template <> struct is_integral_number<char> : std::false_type { };
template <> struct is_integral_number<char16_t> : std::false_type { };
template <> struct is_integral_number<char32_t> : std::false_type { };
template <> struct is_integral_number<wchar_t> : std::false_type { };
#ifdef __cpp_char8_t
template <> struct is_integral_number<char8_t> : std::false_type { };
#endif
// clang-format on

template <class T, bool = is_integral_number<T>::value>
struct converter;

template <class T>
struct converter<T, true> {
  static void apply(T& i, resp3::node_view const& node, std::error_code& ec) {
    auto const& view = node.value();
    auto const res = std::from_chars(view.data(), view.data() + view.size(), i);
    if (res.ec != std::errc()) {
      ec = redisus::error::not_a_number;
    } else if (res.ptr != view.data() + view.size()) {
      ec = redisus::error::invalid_number_format;
    }
  }
};

template <>
struct converter<bool, false> {
  static void apply(bool& t, resp3::node_view const& node, std::error_code&) { t = *node.value().data() == 't'; }
};

template <>
struct converter<double, false> {
  static void apply(double& d, resp3::node_view const& node, std::error_code& ec) {
    auto const& view = node.value();
    auto const res = std::from_chars(view.data(), view.data() + view.size(), d);
    if (res.ec != std::errc()) {
      ec = redisus::error::not_a_double;
    } else if (res.ptr != view.data() + view.size()) {
      ec = redisus::error::invalid_double_format;
    }
  }
};

template <class CharT, class Traits, class Allocator>
struct converter<std::basic_string<CharT, Traits, Allocator>, false> {
  static void apply(std::basic_string<CharT, Traits, Allocator>& s, resp3::node_view const& node, std::error_code&) {
    s.assign(node.value().data(), node.value().size());
  }
};

template <class T>
struct from_bulk_impl {
  static void apply(T& t, resp3::node_view const& node, std::error_code& ec) { converter<T>::apply(t, node, ec); }
};

template <class T>
struct from_bulk_impl<std::optional<T>> {
  static void apply(std::optional<T>& op, resp3::node_view const& node, std::error_code& ec) {
    if (node.data_type != resp3::type3::null) {
      op.emplace(T{});
      converter<T>::apply(op.value(), node, ec);
    }
  }
};

template <class T>
void from_bulk(T& t, resp3::node_view const& node, std::error_code& ec) {
  from_bulk_impl<T>::apply(t, node, ec);
}

}  // namespace redisus::adapter::detail
