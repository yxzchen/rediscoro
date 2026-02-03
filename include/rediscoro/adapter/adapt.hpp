#pragma once

#include <rediscoro/adapter/detail/adapt_array.hpp>
#include <rediscoro/adapter/detail/adapt_ignore.hpp>
#include <rediscoro/adapter/detail/adapt_map.hpp>
#include <rediscoro/adapter/detail/adapt_optional.hpp>
#include <rediscoro/adapter/detail/adapt_scalar.hpp>
#include <rediscoro/adapter/detail/adapt_sequence.hpp>
#include <rediscoro/adapter/detail/traits.hpp>
#include <rediscoro/adapter/error.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/ignore.hpp>
#include <rediscoro/resp3/message.hpp>

#include <string>
#include <string_view>
#include <type_traits>

namespace rediscoro::adapter {

/// Adapt a RESP3 message into a C++ value.
///
/// IMPORTANT: `adapt<T>()` is called from the connection strand (inside response delivery).
/// Therefore, `T` should be *passive*: constructing/appending/emplacing it should not block and
/// should not perform side effects (locks, IO, logging, callbacks).
///
/// This is a contract with the caller (currently not enforced statically).
/// Recommended targets: trivial arithmetic types, `std::string`, and standard containers of
/// passive element types.

template <typename T>
auto adapt(const resp3::message& msg) -> expected<T, error> {
  using U = detail::remove_cvref_t<T>;

  static_assert(
    !detail::is_std_string_view_v<U>,
    "adapter::adapt<std::string_view> is not supported: it would produce dangling views. "
    "Use std::string instead.");

  if constexpr (std::is_same_v<U, ignore_t>) {
    return detail::adapt_ignore<U>(msg);
  } else if constexpr (detail::is_std_optional_v<U>) {
    return detail::adapt_optional<U>(msg);
  } else if constexpr (detail::is_std_array_v<U>) {
    return detail::adapt_std_array<U>(msg);
  } else if constexpr (detail::sequence_like<U> && !detail::is_std_string_v<U>) {
    return detail::adapt_sequence<U>(msg);
  } else if constexpr (detail::map_like<U>) {
    return detail::adapt_map<U>(msg);
  } else {
    return detail::adapt_scalar<U>(msg);
  }
}

}  // namespace rediscoro::adapter
