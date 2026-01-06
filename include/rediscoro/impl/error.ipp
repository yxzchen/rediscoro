#include <rediscoro/assert.hpp>
#include <rediscoro/error.hpp>

namespace rediscoro {
namespace detail {

class error_category_impl : public std::error_category {
 public:
  auto name() const noexcept -> char const* override { return "rediscoro"; }

  auto message(int ev) const -> std::string override {
    switch (static_cast<error>(ev)) {
      case error::operation_aborted:
        return "operation aborted";
      case error::not_connected:
        return "not connected";
      case error::already_in_progress:
        return "already in progress";
      case error::resolve_failed:
        return "resolve failed";
      case error::resolve_timeout:
        return "resolve timeout";
      case error::connect_failed:
        return "connect failed";
      case error::connect_timeout:
        return "connect timeout";
      case error::connection_reset:
        return "connection reset";
      case error::handshake_failed:
        return "handshake failed";
      case error::handshake_timeout:
        return "handshake timeout";
      case error::unsolicited_message:
        return "unsolicited message";
      case error::request_timeout:
        return "request timeout";
      case error::connection_closed:
        return "connection closed";
      case error::connection_lost:
        return "connection lost";
      default:
        return "unknown error";
    }
  }
};

inline auto category() -> std::error_category const& {
  static error_category_impl instance;
  return instance;
}

}  // namespace detail

auto make_error_code(error e) -> std::error_code {
  return std::error_code{static_cast<int>(e), detail::category()};
}

}  // namespace rediscoro
