#pragma once

#include <system_error>
#include <type_traits>

namespace rediscoro {

enum class error {
  /// Operation cancelled by user (close() was called).
  operation_aborted = 1,

  /// Connection not established yet (INIT or CONNECTING state).
  /// User must wait for connect() to complete before enqueuing requests.
  not_connected,

  /// Operation already in progress.
  /// For example, calling connect() while another connect() is already in progress.
  already_in_progress,

  /// Address resolution failed.
  /// DNS resolution or name lookup failed before a TCP connection attempt.
  resolve_failed,

  /// TCP connect failed.
  /// The endpoint was resolved but the TCP connection could not be established.
  connect_failed,

  /// Handshake with Redis server failed.
  /// This can happen during initial connect() or during reconnection.
  /// Reasons: HELLO/AUTH/SELECT command failed, protocol mismatch, authentication error.
  handshake_failed,

  /// Connection timeout.
  /// TCP connection or handshake took too long to complete.
  timeout,

  /// Connection is closed (CLOSING or CLOSED state).
  /// Either user called close() or connection was shut down.
  connection_closed,

  /// Connection lost due to runtime error (FAILED state).
  /// This occurs when an established connection fails during operation.
  /// Automatic reconnection may be in progress in the background.
  connection_lost,
};

auto make_error_code(error e) -> std::error_code;

}  // namespace rediscoro

namespace std {

template <>
struct is_error_code_enum<rediscoro::error> : std::true_type {};

}  // namespace std

#include <rediscoro/impl/error.ipp>
