#pragma once

#include <xz/redis/adapter/detail/wrapper.hpp>
#include <xz/redis/adapter/ignore.hpp>
#include <xz/redis/adapter/result.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/ignore.hpp>
#include <xz/redis/resp3/type.hpp>

namespace xz::redis::adapter::detail {

template <class Result>
struct result_traits {
  using adapter_type = wrapper<typename std::decay<Result>::type>;
  static auto adapt(Result& r) noexcept { return adapter_type{&r}; }
};

template <>
struct result_traits<result<ignore_t>> {
  using response_type = result<ignore_t>;
  using adapter_type = ignore;
  static auto adapt(response_type&) noexcept { return adapter_type{}; }
};

template <class T>
using adapter_t = typename result_traits<std::decay_t<T>>::adapter_type;

}  // namespace xz::redis::adapter::detail
