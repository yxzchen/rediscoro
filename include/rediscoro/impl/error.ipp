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
      case error::connection_closed:
        return "connection closed";
      case error::connection_error:
        return "connection error";
      case error::not_connected:
        return "not connected";
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
