#pragma once

#include <rediscoro/adapter/error.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/resp3/message.hpp>

namespace rediscoro::adapter {

template <typename T>
auto adapt(const resp3::message& msg) -> expected<T, error>;

namespace detail {

template <typename T>
auto adapt_ignore(const resp3::message&) -> expected<T, error> {
  return T{};
}

}  // namespace detail
}  // namespace rediscoro::adapter
