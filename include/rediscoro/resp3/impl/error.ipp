#pragma once

#include <rediscoro/resp3/error.hpp>

namespace rediscoro::resp3 {

class resp3_error_category final : public std::error_category {
public:
  auto name() const noexcept -> const char* override {
    return "resp3";
  }

  auto message(int ev) const -> std::string override {
    switch (static_cast<error>(ev)) {
      case error::needs_more:
        return "needs more";
      case error::invalid_type_byte:
        return "invalid type byte";
      case error::invalid_null:
        return "invalid null";
      case error::invalid_boolean:
        return "invalid boolean";
      case error::invalid_bulk_trailer:
        return "invalid bulk trailer";
      case error::invalid_double:
        return "invalid double";
      case error::invalid_integer:
        return "invalid integer";
      case error::invalid_length:
        return "invalid length";
      case error::invalid_map_pairs:
        return "invalid map pairs";
      case error::invalid_state:
        return "invalid state";
      case error::parser_failed:
        return "parser failed";
      default:
        return "unknown error";
    }
  }
};

inline const resp3_error_category resp3_error_category_instance{};

inline auto make_error_code(error e) -> std::error_code {
  return {static_cast<int>(e), resp3_error_category_instance};
}

}  // namespace rediscoro::resp3
