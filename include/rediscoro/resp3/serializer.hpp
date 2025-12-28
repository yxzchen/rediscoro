#pragma once

#include <rediscoro/assert.hpp>
#include <rediscoro/resp3/parser.hpp>
#include <rediscoro/resp3/type.hpp>

#include <string>
#include <tuple>

namespace rediscoro::resp3 {

void add_header(std::string& payload, type3 type, std::size_t size);
void add_blob(std::string& payload, std::string_view blob);
void add_separator(std::string& payload);

void to_bulk(std::string& payload, std::string_view data);

template <class T, typename = typename std::enable_if<std::is_integral<T>::value>::type>
void to_bulk(std::string& payload, T const& n) {
  auto const s = std::to_string(n);
  to_bulk(payload, std::string_view{s});
}

template <class T>
struct add_bulk_impl {
  static void add(std::string& payload, T const& from) { to_bulk(payload, from); }
};

template <class... Ts>
struct add_bulk_impl<std::tuple<Ts...>> {
  static void add(std::string& payload, std::tuple<Ts...> const& t) {
    auto f = [&](auto const&... vs) { (to_bulk(payload, vs), ...); };

    std::apply(f, t);
  }
};

template <class U, class V>
struct add_bulk_impl<std::pair<U, V>> {
  static void add(std::string& payload, std::pair<U, V> const& from) {
    to_bulk(payload, from.first);
    to_bulk(payload, from.second);
  }
};

template <class T>
void add_bulk(std::string& payload, T const& data) {
  add_bulk_impl<T>::add(payload, data);
}

template <class>
struct bulk_counter {
  static constexpr auto size = 1U;
};

template <class T, class U>
struct bulk_counter<std::pair<T, U>> {
  static constexpr auto size = 2U;
};

template <class... T>
struct bulk_counter<std::tuple<T...>> {
  static constexpr auto size = sizeof...(T);
};

}  // namespace rediscoro::resp3
