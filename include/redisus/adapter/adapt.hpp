#pragma once

#include <redisus/adapter/detail/response_traits.hpp>
#include <redisus/adapter/detail/result_traits.hpp>

namespace boost::redis::adapter {

template <class T>
auto adapt_resp(T& t) noexcept {
  return detail::response_traits<T>::adapt(t);
}

template <class T>
auto adapt2(T& t = redis::ignore) noexcept {
  return detail::result_traits<T>::adapt(t);
}

}  // namespace boost::redis::adapter
