#pragma once

#include <system_error>
#include <type_traits>

namespace rediscoro {

// New error domains (preferred public API):
// - client_errc: lifecycle / IO / timeout / cancellation / gating
// - protocol_errc: RESP3 protocol parse/validation errors (no "needs more" internal signal)
// - server_errc: Redis error replies
// - adapter_errc: type adaptation failures

enum class client_errc {
  /// Operation cancelled by user (close() was called).
  operation_aborted = 1,

  /// Connection is closed (CLOSING or CLOSED state).
  connection_closed,

  /// DNS resolution failed.
  resolve_failed,

  /// DNS resolution timed out.
  resolve_timeout,

  /// TCP connect failed.
  connect_failed,

  /// TCP connect timed out.
  connect_timeout,

  /// Connection reset / peer closed.
  connection_reset,

  /// Handshake failed (HELLO/AUTH/SELECT/SETNAME, protocol mismatch, auth error, etc).
  handshake_failed,

  /// Handshake timed out.
  handshake_timeout,

  /// Socket write error.
  write_error,

  /// Connection lost due to runtime error (FAILED state).
  connection_lost,

  /// Server sent an unsolicited message (e.g. PUSH) or unexpected message arrived.
  unsolicited_message,

  /// Request timed out (connection-level policy; may trigger reconnect).
  request_timeout,

  /// Connection not established yet (INIT or CONNECTING state).
  not_connected,

  /// Operation already in progress.
  already_in_progress,

  /// Local request queue hit backpressure limits.
  queue_full,

  /// Internal error (bug / invariant violation).
  internal_error,
};

enum class protocol_errc {
  /// Attempted to parse but prior parsed tree was not reclaimed/consumed.
  tree_not_consumed = 1,

  /// RESP3 type byte is invalid.
  invalid_type_byte,

  /// RESP3 null format is invalid.
  invalid_null,

  /// RESP3 boolean format is invalid.
  invalid_boolean,

  /// RESP3 bulk string/error trailer is invalid (missing \\r\\n).
  invalid_bulk_trailer,

  /// RESP3 double format is invalid.
  invalid_double,

  /// RESP3 integer format is invalid.
  invalid_integer,

  /// RESP3 length field is invalid (negative or malformed).
  invalid_length,

  /// RESP3 map has mismatched key-value pairs.
  invalid_map_pairs,

  /// Parser internal state is invalid (should not happen, indicates a bug).
  invalid_state,

  /// Parser is in failed state (prior protocol error occurred).
  parser_failed,

  /// RESP3 verbatim payload must be "xxx:<data>" (3-byte encoding + ':').
  invalid_verbatim,
};

enum class server_errc {
  /// Redis replied with an error value (simple_error / bulk_error).
  redis_error = 1,
};

enum class adapter_errc {
  /// Adaptation failed due to a type mismatch.
  type_mismatch = 1,
  /// Adaptation failed due to an unexpected null.
  unexpected_null,
  /// Adaptation failed due to out-of-range numeric conversion.
  value_out_of_range,
  /// Adaptation failed due to duplicate keys in map target.
  duplicate_key,
  /// Adaptation failed due to size mismatch (array / container).
  size_mismatch,
};

auto make_error_code(client_errc e) -> std::error_code;
auto make_error_code(protocol_errc e) -> std::error_code;
auto make_error_code(server_errc e) -> std::error_code;
auto make_error_code(adapter_errc e) -> std::error_code;

[[nodiscard]] auto is_client_error(std::error_code ec) noexcept -> bool;
[[nodiscard]] auto is_protocol_error(std::error_code ec) noexcept -> bool;

[[nodiscard]] auto is_timeout(std::error_code ec) noexcept -> bool;
[[nodiscard]] auto is_retryable(std::error_code ec) noexcept -> bool;

}  // namespace rediscoro

namespace std {

template <>
struct is_error_code_enum<rediscoro::client_errc> : std::true_type {};

template <>
struct is_error_code_enum<rediscoro::protocol_errc> : std::true_type {};

template <>
struct is_error_code_enum<rediscoro::server_errc> : std::true_type {};

template <>
struct is_error_code_enum<rediscoro::adapter_errc> : std::true_type {};

}  // namespace std

#include <rediscoro/impl/error.ipp>
