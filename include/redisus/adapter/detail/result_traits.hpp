#pragma once

#include <redisus/adapter/detail/wrapper.hpp>
#include <redisus/adapter/ignore.hpp>
#include <redisus/adapter/result.hpp>
#include <redisus/error.hpp>
#include <redisus/ignore.hpp>
#include <redisus/resp3/type.hpp>

namespace redisus::adapter::detail {

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

// template <>
// struct result_traits<result<resp3::msg_view>> {
//   using response_type = result<resp3::msg_view>;
//   using adapter_type = adapter::detail::general_msg<response_type>;
//   static auto adapt(response_type& v) noexcept { return adapter_type{&v}; }
// };

template <class T>
using adapter_t = typename result_traits<std::decay_t<T>>::adapter_type;

}  // namespace redisus::adapter::detail
