#pragma once

#include <rediscoro/adapter/detail/traits.hpp>
#include <rediscoro/adapter/error.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/resp3/message.hpp>

#include <cstddef>
#include <tuple>
#include <utility>

namespace rediscoro::adapter {

template <typename T>
auto adapt(const resp3::message& msg) -> expected<T, error>;

namespace detail {

template <typename T>
auto adapt_std_array(const resp3::message& msg) -> expected<T, error> {
  using U = remove_cvref_t<T>;
  using V = typename U::value_type;
  constexpr std::size_t N = std::tuple_size_v<U>;

  if (!msg.is<resp3::array>()) {
    return unexpected(detail::make_type_mismatch(msg.get_kind(), {resp3::kind::array}));
  }
  const auto& elems = msg.as<resp3::array>().elements;
  if (elems.size() != N) {
    return unexpected(detail::make_size_mismatch(msg.get_kind(), N, elems.size()));
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
}  // namespace rediscoro::adapter
