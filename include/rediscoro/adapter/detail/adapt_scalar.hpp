#pragma once

#include <rediscoro/adapter/detail/traits.hpp>
#include <rediscoro/adapter/error.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/resp3/message.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace rediscoro::adapter {

template <typename T>
auto adapt(const resp3::message& msg) -> expected<T, error>;

namespace detail {

template <typename T>
auto adapt_scalar(const resp3::message& msg) -> expected<T, error> {
  using U = remove_cvref_t<T>;

  if constexpr (is_std_string_v<U>) {
    if (msg.is<resp3::null>()) {
      return unexpected(make_unexpected_null(
        {resp3::kind::simple_string, resp3::kind::bulk_string, resp3::kind::verbatim_string}));
    }
    if (msg.is<resp3::simple_string>()) {
      return std::string{msg.as<resp3::simple_string>().data};
    }
    if (msg.is<resp3::bulk_string>()) {
      return std::string{msg.as<resp3::bulk_string>().data};
    }
    if (msg.is<resp3::verbatim_string>()) {
      return std::string{msg.as<resp3::verbatim_string>().data};
    }
    return unexpected(make_type_mismatch(
      msg.get_kind(),
      {resp3::kind::simple_string, resp3::kind::bulk_string, resp3::kind::verbatim_string}));
  } else if constexpr (integral_like<U>) {
    if (msg.is<resp3::null>()) {
      return unexpected(make_unexpected_null({resp3::kind::integer}));
    }
    if (!msg.is<resp3::integer>()) {
      return unexpected(make_type_mismatch(msg.get_kind(), {resp3::kind::integer}));
    }
    const auto v = msg.as<resp3::integer>().value;
    if (v < static_cast<std::int64_t>((std::numeric_limits<U>::min)()) ||
        v > static_cast<std::int64_t>((std::numeric_limits<U>::max)())) {
      return unexpected(make_value_out_of_range(resp3::kind::integer));
    }
    return static_cast<U>(v);
  } else if constexpr (bool_like<U>) {
    if (msg.is<resp3::null>()) {
      return unexpected(make_unexpected_null({resp3::kind::boolean}));
    }
    if (!msg.is<resp3::boolean>()) {
      return unexpected(make_type_mismatch(msg.get_kind(), {resp3::kind::boolean}));
    }
    return msg.as<resp3::boolean>().value;
  } else if constexpr (double_like<U>) {
    if (msg.is<resp3::null>()) {
      return unexpected(make_unexpected_null({resp3::kind::double_number}));
    }
    if (!msg.is<resp3::double_number>()) {
      return unexpected(make_type_mismatch(msg.get_kind(), {resp3::kind::double_number}));
    }
    return static_cast<U>(msg.as<resp3::double_number>().value);
  } else {
    static_assert(dependent_false_v<U>, "no scalar adapter for this type");
  }
}

}  // namespace detail
}  // namespace rediscoro::adapter
