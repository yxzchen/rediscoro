#pragma once

#include <rediscoro/error.hpp>
#include <rediscoro/resp3/node.hpp>

#include <charconv>
#include <concepts>
#include <optional>
#include <type_traits>

namespace rediscoro::adapter::detail {

template <class T>
concept is_number = std::integral<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, char> &&
                      !std::is_same_v<T, char16_t> && !std::is_same_v<T, char32_t> &&
                      !std::is_same_v<T, wchar_t>
#ifdef __cpp_char8_t
                      && !std::is_same_v<T, char8_t>
#endif
;

template <class T, bool = is_number<T>>
struct converter;

template <class T>
struct converter<T, true> {
  static void apply(T& i, resp3::node_view const& node, std::error_code& ec) {
    auto const& view = node.value();
    auto const res = std::from_chars(view.data(), view.data() + view.size(), i);
    if (res.ec != std::errc() || res.ptr != view.data() + view.size()) {
      ec = rediscoro::error::not_a_number;
    }
  }
};

template <>
struct converter<bool, false> {
  static void apply(bool& t, resp3::node_view const& node, std::error_code& ec) {
    auto const& view = node.value();
    if (view == "t") {
      t = true;
      return;
    }
    if (view == "f") {
      t = false;
      return;
    }
    ec = rediscoro::error::unexpected_bool_value;
  }
};

template <>
struct converter<double, false> {
  static void apply(double& d, resp3::node_view const& node, std::error_code& ec) {
    auto const& view = node.value();
    auto const res = std::from_chars(view.data(), view.data() + view.size(), d);
    if (res.ec != std::errc() || res.ptr != view.data() + view.size()) {
      ec = rediscoro::error::not_a_double;
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

}  // namespace rediscoro::adapter::detail
