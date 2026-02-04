#pragma once

#include <rediscoro/adapter/detail/traits.hpp>
#include <rediscoro/adapter/error.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/ignore.hpp>
#include <rediscoro/resp3/message.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace rediscoro::adapter {

template <typename T>
auto adapt(const resp3::message& msg) -> expected<T, error>;

namespace detail {

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
    return unexpected(detail::make_type_mismatch(msg.get_kind(), {resp3::kind::map}));
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
      if constexpr (std::is_same_v<remove_cvref_t<K>, std::string>) {
        e.prepend_path(path_key{*rk});
      } else {
        e.prepend_path(path_field{"value"});
        e.prepend_path(path_index{i});
      }
      return unexpected(std::move(e));
    }
    out.emplace(std::move(*rk), std::move(*rv));
  }
  return out;
}

}  // namespace detail
}  // namespace rediscoro::adapter
