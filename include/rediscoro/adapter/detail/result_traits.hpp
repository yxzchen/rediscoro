#pragma once

#include <rediscoro/adapter/detail/wrapper.hpp>
#include <rediscoro/adapter/ignore.hpp>
#include <rediscoro/adapter/result.hpp>
#include <rediscoro/error.hpp>
#include <rediscoro/ignore.hpp>
#include <rediscoro/resp3/type.hpp>

namespace rediscoro::adapter::detail {

template <class Result>
struct result_traits {
  using adapter_type = wrapper<typename std::decay<Result>::type>;
  static auto adapt(Result& r) noexcept { return adapter_type{&r}; }
};

template <>
struct result_traits<result<ignore_t>> {
  using response_type = result<ignore_t>;
  using adapter_type = ignore;
  static auto adapt(response_type& r) noexcept { return adapter_type{&r}; }
};

template <class T>
using adapter_t = typename result_traits<std::decay_t<T>>::adapter_type;

}  // namespace rediscoro::adapter::detail
