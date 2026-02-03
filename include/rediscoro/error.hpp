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
  /// Peer closed the connection (EOF) or reset it.
  connection_reset,

  /// Handshake with Redis server failed.
  /// This can happen during initial connect() or during reconnection.
  /// Reasons: HELLO/AUTH/SELECT command failed, protocol mismatch, authentication error.
  handshake_failed,

  /// RESP3 handshake timed out.
  /// Handshake did not complete within config::connect_timeout.
  handshake_timeout,

  /// Server sent a message we don't support (e.g. RESP3 PUSH) or an unexpected message arrived.
  /// Current policy: treated as fatal and triggers reconnect.
  unsolicited_message,

  /// Request timed out.
  /// Request timeout is connection-level: triggers reconnect.
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
  /// Internal-only signal between parser and connection: read more bytes and retry parsing.
  resp3_needs_more = 100,

  /// Attempted to parse but tree was not consumed (reclaim not called).
  resp3_tree_not_consumed,

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

  /// Parser is in failed state (prior protocol error occurred).
  resp3_parser_failed,
};

auto make_error_code(error e) -> std::error_code;

/// Check if an error is an internal state that should not be exposed to users.
/// These should not appear in user-visible responses.
constexpr bool is_internal_error(error e) noexcept {
  return e == error::resp3_needs_more || e == error::resp3_tree_not_consumed;
}

}  // namespace rediscoro

namespace std {

template <>
struct is_error_code_enum<rediscoro::error> : std::true_type {};

}  // namespace std

#include <rediscoro/impl/error.ipp>
