#include <rediscoro/assert.hpp>
#include <rediscoro/error.hpp>

namespace rediscoro {
namespace detail {

struct error_category_impl : std::error_category {
  virtual ~error_category_impl() = default;

  auto name() const noexcept -> char const* override {
    return "rediscoro";
  }

  auto message(int ev) const -> std::string override {
    // clang-format off
    switch (static_cast<error>(ev)) {
      case error::invalid_data_type: return "Invalid resp3 type.";
      case error::not_a_number:
        return "Can't convert string to number (maybe forgot to upgrade to RESP3?).";
      case error::exceeeds_max_nested_depth:
        return "Exceeds the maximum number of nested responses.";
      case error::aggregate_size_overflow:        return "Aggregate size would cause integer overflow.";
      case error::unexpected_bool_value:          return "Unexpected bool value.";
      case error::empty_field:                    return "Expected field value is empty.";
      case error::expects_resp3_simple_type:      return "Expects a resp3 simple type.";
      case error::expects_resp3_aggregate:        return "Expects resp3 aggregate.";
      case error::expects_resp3_map:              return "Expects resp3 map.";
      case error::expects_resp3_set:              return "Expects resp3 set.";
      case error::nested_aggregate_not_supported: return "Nested aggregate not_supported.";
      case error::resp3_simple_error:             return "Got RESP3 simple-error.";
      case error::resp3_blob_error:               return "Got RESP3 blob-error.";
      case error::incompatible_size:              return "Aggregate container has incompatible size.";
      case error::not_a_double:                   return "Not a double.";
      case error::resp3_null:                     return "Got RESP3 null.";
      case error::not_connected:                  return "Not connected.";
      case error::resolve_timeout:                return "Resolve timeout.";
      case error::connect_timeout:                return "Connect timeout.";
      case error::pong_timeout:                   return "Pong timeout.";
      case error::ssl_handshake_timeout:          return "SSL handshake timeout.";
      case error::sync_receive_push_failed:
        return "Can't receive server push synchronously without blocking.";
      case error::incompatible_node_depth: return "Incompatible node depth.";
      case error::unix_sockets_unsupported:
        return "The configuration specified a UNIX socket address, but UNIX sockets are not "
               "supported by the system.";
      case error::unix_sockets_ssl_unsupported:
        return "The configuration specified UNIX sockets with SSL, which is not supported.";
      case error::exceeds_maximum_read_buffer_size:
        return "Reading data from the socket would exceed the maximum size allowed of the read "
               "buffer.";
      case error::write_timeout:
        return "Timeout while writing data to the server.";
      case error::handshake_error:
        return "Handshake error.";
      default: REDISCORO_ASSERT(false); return "rediscoro error.";
    }
    // clang-format on
  }
};

auto category() -> std::error_category const& {
  static error_category_impl instance;
  return instance;
}

}  // namespace detail

auto make_error_code(error e) -> std::error_code {
  return std::error_code{static_cast<int>(e), detail::category()};
}

}  // namespace rediscoro
