#pragma once

#include <rediscoro/adapter/detail/traits.hpp>
#include <rediscoro/adapter/error.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/ignore.hpp>
#include <rediscoro/resp3/message.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace rediscoro::adapter {

template <typename T>
auto adapt(const resp3::message& msg) -> expected<T, error>;

namespace detail {

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
    return unexpected(detail::make_type_mismatch(
      msg.get_kind(), {resp3::kind::array, resp3::kind::set, resp3::kind::push}));
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

}  // namespace detail
}  // namespace rediscoro::adapter

