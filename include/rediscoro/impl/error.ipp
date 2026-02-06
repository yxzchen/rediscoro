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
      case client_errc::not_connected:
        return "not connected";
      case client_errc::already_in_progress:
        return "already in progress";
      case client_errc::resolve_failed:
        return "resolve failed";
      case client_errc::resolve_timeout:
        return "resolve timeout";
      case client_errc::connect_failed:
        return "connect failed";
      case client_errc::connect_timeout:
        return "connect timeout";
      case client_errc::connection_refused:
        return "connection refused";
      case client_errc::connection_timed_out:
        return "connection timed out";
      case client_errc::connection_aborted:
        return "connection aborted";
      case client_errc::network_unreachable:
        return "network unreachable";
      case client_errc::host_unreachable:
        return "host unreachable";
      case client_errc::address_in_use:
        return "address in use";
      case client_errc::connection_reset:
        return "connection reset";
      case client_errc::handshake_failed:
        return "handshake failed";
      case client_errc::handshake_timeout:
        return "handshake timeout";
      case client_errc::unsolicited_message:
        return "unsolicited message";
      case client_errc::request_timeout:
        return "request timeout";
      case client_errc::connection_closed:
        return "connection closed";
      case client_errc::connection_lost:
        return "connection lost";
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

[[nodiscard]] inline auto is_timeout(std::error_code ec) noexcept -> bool {
  if (ec.category() == detail::client_category()) {
    const auto e = static_cast<client_errc>(ec.value());
    if (e == client_errc::resolve_timeout || e == client_errc::connect_timeout ||
        e == client_errc::connection_timed_out || e == client_errc::handshake_timeout ||
        e == client_errc::request_timeout) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] inline auto is_client_error(std::error_code ec) noexcept -> bool {
  return ec.category() == detail::client_category();
}

[[nodiscard]] inline auto is_protocol_error(std::error_code ec) noexcept -> bool {
  return ec.category() == detail::protocol_category();
}

[[nodiscard]] inline auto is_retryable(std::error_code ec) noexcept -> bool {
  if (ec.category() == detail::client_category()) {
    const auto e = static_cast<client_errc>(ec.value());
    if (e == client_errc::connection_lost || e == client_errc::connection_reset ||
        e == client_errc::request_timeout || e == client_errc::handshake_failed ||
        e == client_errc::unsolicited_message || e == client_errc::connection_timed_out ||
        e == client_errc::network_unreachable || e == client_errc::host_unreachable) {
      return true;
    }
    // connection_refused is usually a configuration issue, not retryable
    if (e == client_errc::connection_refused || e == client_errc::connection_aborted ||
        e == client_errc::address_in_use) {
      return false;
    }
    return false;
  }
  if (ec.category() == detail::protocol_category()) {
    return true;  // protocol errors are retryable via reconnect (policy-dependent)
  }
  return false;
}

[[nodiscard]] inline auto action_hint(std::error_code ec) noexcept -> error_action_hint {
  if (!ec) {
    return error_action_hint::none;
  }
  if (ec.category() == detail::protocol_category()) {
    return error_action_hint::reconnect;
  }
  if (ec.category() == detail::adapter_category()) {
    return error_action_hint::fix_input;
  }
  if (ec.category() == detail::server_category()) {
    // Server error replies are usually request/data dependent.
    return error_action_hint::fix_input;
  }
  if (ec.category() == detail::client_category()) {
    const auto e = static_cast<client_errc>(ec.value());
    switch (e) {
      case client_errc::operation_aborted: {
        return error_action_hint::none;
      }
      case client_errc::not_connected:
      case client_errc::connection_closed:
      case client_errc::connection_lost:
      case client_errc::connection_reset:
      case client_errc::handshake_failed:
      case client_errc::handshake_timeout:
      case client_errc::request_timeout:
      case client_errc::unsolicited_message:
      case client_errc::connection_timed_out:
      case client_errc::network_unreachable:
      case client_errc::host_unreachable: {
        return error_action_hint::reconnect;
      }
      case client_errc::already_in_progress: {
        return error_action_hint::retry_request;
      }
      case client_errc::resolve_failed:
      case client_errc::resolve_timeout:
      case client_errc::connect_failed:
      case client_errc::connect_timeout:
      case client_errc::connection_refused:
      case client_errc::connection_aborted:
      case client_errc::address_in_use: {
        return error_action_hint::reconnect;
      }
      case client_errc::internal_error: {
        return error_action_hint::bug;
      }
    }
  }
  return error_action_hint::none;
}

[[nodiscard]] inline auto is_transient(std::error_code ec) noexcept -> bool {
  if (!ec) {
    return false;
  }
  if (ec.category() == detail::client_category()) {
    const auto e = static_cast<client_errc>(ec.value());
    switch (e) {
      case client_errc::resolve_timeout:
      case client_errc::connect_timeout:
      case client_errc::connection_timed_out:
      case client_errc::handshake_timeout:
      case client_errc::request_timeout:
      case client_errc::connection_reset:
      case client_errc::connection_lost:
      case client_errc::network_unreachable:
      case client_errc::host_unreachable: {
        return true;
      }
      case client_errc::operation_aborted:
      case client_errc::not_connected:
      case client_errc::already_in_progress:
      case client_errc::resolve_failed:
      case client_errc::connect_failed:
      case client_errc::connection_refused:
      case client_errc::connection_aborted:
      case client_errc::address_in_use:
      case client_errc::handshake_failed:
      case client_errc::unsolicited_message:
      case client_errc::connection_closed:
      case client_errc::internal_error: {
        return false;
      }
    }
  }
  // Protocol errors are usually fatal to the current connection but transient across reconnect.
  if (ec.category() == detail::protocol_category()) {
    return true;
  }
  return false;
}

[[nodiscard]] inline auto is_reconnect_required(std::error_code ec) noexcept -> bool {
  if (!ec) {
    return false;
  }
  if (ec.category() == detail::protocol_category()) {
    return true;
  }
  if (ec.category() == detail::client_category()) {
    const auto e = static_cast<client_errc>(ec.value());
    switch (e) {
      case client_errc::connection_reset:
      case client_errc::connection_lost:
      case client_errc::handshake_failed:
      case client_errc::handshake_timeout:
      case client_errc::request_timeout:
      case client_errc::unsolicited_message:
      case client_errc::connection_timed_out:
      case client_errc::network_unreachable:
      case client_errc::host_unreachable: {
        return true;
      }
      case client_errc::operation_aborted:
      case client_errc::not_connected:
      case client_errc::already_in_progress:
      case client_errc::resolve_failed:
      case client_errc::resolve_timeout:
      case client_errc::connect_failed:
      case client_errc::connect_timeout:
      case client_errc::connection_refused:
      case client_errc::connection_aborted:
      case client_errc::address_in_use:
      case client_errc::connection_closed:
      case client_errc::internal_error: {
        return false;
      }
    }
  }
  return false;
}

}  // namespace rediscoro
