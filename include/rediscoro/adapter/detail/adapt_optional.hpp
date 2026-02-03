#pragma once

#include <rediscoro/adapter/detail/traits.hpp>
#include <rediscoro/adapter/error.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/resp3/message.hpp>

#include <optional>
#include <utility>

namespace rediscoro::adapter {

template <typename T>
auto adapt(const resp3::message& msg) -> expected<T, error>;

namespace detail {

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

}  // namespace detail
}  // namespace rediscoro::adapter

