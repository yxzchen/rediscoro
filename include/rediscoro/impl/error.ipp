#include <rediscoro/assert.hpp>
#include <rediscoro/error.hpp>

namespace rediscoro {
namespace detail {

class client_category_impl : public std::error_category {
 public:
  auto name() const noexcept -> char const* override { return "rediscoro.client"; }

  auto message(int ev) const -> std::string override {
    switch (static_cast<client_errc>(ev)) {
      case client_errc::operation_aborted:
        return "operation aborted";
      case client_errc::connection_closed:
        return "connection closed";
      case client_errc::resolve_failed:
        return "resolve failed";
      case client_errc::resolve_timeout:
        return "resolve timeout";
      case client_errc::connect_failed:
        return "connect failed";
      case client_errc::connect_timeout:
        return "connect timeout";
      case client_errc::connection_reset:
        return "connection reset";
      case client_errc::handshake_failed:
        return "handshake failed";
      case client_errc::handshake_timeout:
        return "handshake timeout";
      case client_errc::write_error:
        return "write error";
      case client_errc::connection_lost:
        return "connection lost";
      case client_errc::unsolicited_message:
        return "unsolicited message";
      case client_errc::request_timeout:
        return "request timeout";
      case client_errc::not_connected:
        return "not connected";
      case client_errc::already_in_progress:
        return "already in progress";
      case client_errc::internal_error:
        return "internal error";
    }
    return "unknown client error";
  }
};

class protocol_category_impl : public std::error_category {
 public:
  auto name() const noexcept -> char const* override { return "rediscoro.resp3"; }

  auto message(int ev) const -> std::string override {
    switch (static_cast<protocol_errc>(ev)) {
      case protocol_errc::tree_not_consumed:
        return "resp3 tree not consumed";
      case protocol_errc::invalid_type_byte:
        return "resp3 invalid type byte";
      case protocol_errc::invalid_null:
        return "resp3 invalid null";
      case protocol_errc::invalid_boolean:
        return "resp3 invalid boolean";
      case protocol_errc::invalid_bulk_trailer:
        return "resp3 invalid bulk trailer";
      case protocol_errc::invalid_double:
        return "resp3 invalid double";
      case protocol_errc::invalid_integer:
        return "resp3 invalid integer";
      case protocol_errc::invalid_length:
        return "resp3 invalid length";
      case protocol_errc::invalid_map_pairs:
        return "resp3 invalid map pairs";
      case protocol_errc::invalid_state:
        return "resp3 invalid state";
      case protocol_errc::parser_failed:
        return "resp3 parser failed";
    }
    return "unknown protocol error";
  }
};

class server_category_impl : public std::error_category {
 public:
  auto name() const noexcept -> char const* override { return "rediscoro.server"; }

  auto message(int ev) const -> std::string override {
    switch (static_cast<server_errc>(ev)) {
      case server_errc::redis_error:
        return "redis error reply";
    }
    return "unknown server error";
  }
};

class adapter_category_impl : public std::error_category {
 public:
  auto name() const noexcept -> char const* override { return "rediscoro.adapter"; }

  auto message(int ev) const -> std::string override {
    switch (static_cast<adapter_errc>(ev)) {
      case adapter_errc::type_mismatch:
        return "type mismatch";
      case adapter_errc::unexpected_null:
        return "unexpected null";
      case adapter_errc::value_out_of_range:
        return "value out of range";
      case adapter_errc::size_mismatch:
        return "size mismatch";
    }
    return "unknown adapter error";
  }
};

inline auto client_category() -> std::error_category const& {
  static client_category_impl instance;
  return instance;
}

inline auto protocol_category() -> std::error_category const& {
  static protocol_category_impl instance;
  return instance;
}

inline auto server_category() -> std::error_category const& {
  static server_category_impl instance;
  return instance;
}

inline auto adapter_category() -> std::error_category const& {
  static adapter_category_impl instance;
  return instance;
}

}  // namespace detail

inline auto make_error_code(client_errc e) -> std::error_code {
  return {static_cast<int>(e), detail::client_category()};
}

inline auto make_error_code(protocol_errc e) -> std::error_code {
  return {static_cast<int>(e), detail::protocol_category()};
}

inline auto make_error_code(server_errc e) -> std::error_code {
  return {static_cast<int>(e), detail::server_category()};
}

inline auto make_error_code(adapter_errc e) -> std::error_code {
  return {static_cast<int>(e), detail::adapter_category()};
}

[[nodiscard]] inline auto is_client_error(std::error_code ec) noexcept -> bool {
  return ec.category() == detail::client_category();
}

[[nodiscard]] inline auto is_protocol_error(std::error_code ec) noexcept -> bool {
  return ec.category() == detail::protocol_category();
}

/// Returns true if the error is a timeout-related error.
[[nodiscard]] inline auto is_timeout(std::error_code ec) noexcept -> bool {
  if (ec.category() == detail::client_category()) {
    const auto e = static_cast<client_errc>(ec.value());
    return e == client_errc::resolve_timeout || e == client_errc::connect_timeout ||
           e == client_errc::handshake_timeout || e == client_errc::request_timeout;
  }
  return false;
}

/// Returns true if the error may be recoverable by retrying (with or without reconnect).
/// - Connection/IO errors: retry after reconnect
/// - Protocol errors: retry after reconnect
/// - Server errors: usually not retryable without fixing the request
/// - Adapter errors: not retryable (input type mismatch)
[[nodiscard]] inline auto is_retryable(std::error_code ec) noexcept -> bool {
  if (ec.category() == detail::client_category()) {
    const auto e = static_cast<client_errc>(ec.value());
    return e == client_errc::connection_lost || e == client_errc::write_error ||
           e == client_errc::connection_reset || e == client_errc::request_timeout ||
           e == client_errc::handshake_failed || e == client_errc::unsolicited_message;
  }
  if (ec.category() == detail::protocol_category()) {
    return true;  // protocol errors are recoverable via reconnect
  }
  return false;
}

}  // namespace rediscoro
