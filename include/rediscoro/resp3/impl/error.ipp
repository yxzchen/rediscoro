#pragma once

#include <rediscoro/resp3/error.hpp>

namespace rediscoro::resp3 {

namespace {

struct error_category : std::error_category {
  auto name() const noexcept -> const char* override {
    return "resp3";
  }

  auto message(int ev) const -> std::string override {
    switch (static_cast<error>(ev)) {
      case error::needs_more:
        return "needs more";
      case error::invalid_type_byte:
        return "invalid type byte";
      case error::invalid_format:
        return "invalid format";
      case error::invalid_integer:
        return "invalid integer";
      case error::invalid_length:
        return "invalid length";
      case error::nesting_too_deep:
        return "nesting too deep";
      default:
        return "unknown error";
    }
  }
};

const error_category error_category_instance{};

}  // namespace

inline auto make_error_code(error e) -> std::error_code {
  return {static_cast<int>(e), error_category_instance};
}

}  // namespace rediscoro::resp3
