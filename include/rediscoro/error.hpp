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

  /// Address resolution timed out.
  /// DNS resolution (getaddrinfo) did not complete within config::resolve_timeout.
  resolve_timeout,

  /// TCP connect failed.
  /// The endpoint was resolved but the TCP connection could not be established.
  connect_failed,

  /// TCP connect timed out.
  /// TCP connection did not complete within config::connect_timeout.
  connect_timeout,

  /// Connection reset / peer closed.
  ///
  /// Used when the peer closes the TCP connection (EOF) or resets it.
  connection_reset,

  /// Handshake with Redis server failed.
  /// This can happen during initial connect() or during reconnection.
  /// Reasons: HELLO/AUTH/SELECT command failed, protocol mismatch, authentication error.
  handshake_failed,

  /// RESP3 handshake timed out.
  /// Handshake did not complete within config::connect_timeout.
  handshake_timeout,

  /// Server sent a message we don't support (e.g. RESP3 PUSH) or an unexpected message arrived.
  ///
  /// Current policy: treated as fatal and triggers reconnect.
  unsolicited_message,

  /// Request timed out.
  ///
  /// Contract:
  /// - Request timeout is connection-level: once any request times out, the connection is treated
  ///   as unhealthy and enters reconnection.
  request_timeout,

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
