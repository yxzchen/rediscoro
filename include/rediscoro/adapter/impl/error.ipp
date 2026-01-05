#pragma once

#include <rediscoro/adapter/error.hpp>

namespace rediscoro::adapter {

class adapter_error_category final : public std::error_category {
 public:
  auto name() const noexcept -> const char* override { return "adapter"; }

  [[nodiscard]] auto message(int ev) const -> std::string override {
    switch (static_cast<error>(ev)) {
      case error::type_mismatch:
        return "wrong type";
      case error::unexpected_null:
        return "unexpected null";
      case error::value_out_of_range:
        return "value out of range";
      case error::size_mismatch:
        return "size mismatch";
      case error::invalid_value:
        return "invalid value";
    }
    return "unknown error";
  }
};

inline const adapter_error_category adapter_error_category_instance{};

inline auto make_error_code(error e) -> std::error_code {
  return {static_cast<int>(e), adapter_error_category_instance};
}

}  // namespace rediscoro::adapter
