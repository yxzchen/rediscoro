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

  /// Parser needs more data to complete parsing.
  ///
  /// Internal implementation detail: This error code is only used internally
  /// between parser and connection. It will never appear in user-visible response_error.
  ///
  /// How it works:
  /// - parser.parse_one() returns this error when buffer has insufficient data
  /// - connection detects this error and continues reading from socket
  /// - Real protocol errors are passed to pipeline and user
  ///
  /// Analogy: POSIX EAGAIN/EWOULDBLOCK - while in errno space, usually handled
  /// internally by libraries, rarely used directly by applications.
  resp3_needs_more = 100,

  /// RESP3 type byte is invalid (first byte is not a valid RESP3 type marker).
  resp3_invalid_type_byte,

  /// RESP3 null format is invalid.
  resp3_invalid_null,

  /// RESP3 boolean format is invalid.
  resp3_invalid_boolean,

  /// RESP3 bulk string/error trailer is invalid (missing \r\n).
  resp3_invalid_bulk_trailer,

  /// RESP3 double format is invalid.
  resp3_invalid_double,

  /// RESP3 integer format is invalid.
  resp3_invalid_integer,

  /// RESP3 length field is invalid (negative or malformed).
  resp3_invalid_length,

  /// RESP3 map has mismatched key-value pairs.
  resp3_invalid_map_pairs,

  /// Parser internal state is invalid (should not happen, indicates a bug).
  resp3_invalid_state,

  /// Attempted to parse but tree was not consumed (reclaim not called).
  resp3_tree_not_consumed,

  /// Parser is in failed state (prior protocol error occurred).
  resp3_parser_failed,
};

auto make_error_code(error e) -> std::error_code;

/// Check if an error is an internal state that should not be exposed to users.
///
/// Currently only resp3_needs_more is an internal error.
constexpr bool is_internal_error(error e) noexcept {
  return e == error::resp3_needs_more;
}

}  // namespace rediscoro

namespace std {

template <>
struct is_error_code_enum<rediscoro::error> : std::true_type {};

}  // namespace std

#include <rediscoro/impl/error.ipp>
