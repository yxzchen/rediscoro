#pragma once

#include <system_error>
#include <type_traits>

namespace rediscoro {

enum class error {
  /// Operation cancelled.
  operation_aborted = 1,

  /// Connection is not usable (closed by user or reached terminal CLOSED state).
  connection_closed = 2,

  /// Connection failed (IO error, handshake failure, etc.) and request cannot be completed.
  connection_error = 3,
};

auto make_error_code(error e) -> std::error_code;

}  // namespace rediscoro

namespace std {

template <>
struct is_error_code_enum<rediscoro::error> : std::true_type {};

}  // namespace std

#include <rediscoro/impl/error.ipp>
