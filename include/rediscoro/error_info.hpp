#pragma once

#include <rediscoro/error.hpp>

#include <system_error>
#include <string>
#include <string_view>
#include <type_traits>

namespace rediscoro {

/// A compact error object with:
/// - a stable error_code (domain + value)
/// - an optional detail string (human-oriented; may include context)
struct error_info {
  std::error_code code{};
  std::string detail{};

  error_info() = default;

  error_info(std::error_code c) : code(c) {}

  error_info(std::error_code c, std::string d) : code(c), detail(std::move(d)) {}

  template <typename Errc>
    requires(!std::is_same_v<std::remove_cvref_t<Errc>, std::error_code> &&
             requires(Errc e) { std::error_code{e}; })
  error_info(Errc e) : code(std::error_code{e}) {}

  template <typename Errc>
    requires(!std::is_same_v<std::remove_cvref_t<Errc>, std::error_code> &&
             requires(Errc e) { std::error_code{e}; })
  error_info(Errc e, std::string d) : code(std::error_code{e}), detail(std::move(d)) {}

  auto append_detail(std::string_view s) -> error_info& {
    if (s.empty()) {
      return *this;
    }
    if (!detail.empty()) {
      detail += " ";
    }
    detail.append(s.data(), s.size());
    return *this;
  }

  [[nodiscard]] auto to_string() const -> std::string {
    std::string out;

    // Headline: category + message.
    if (code) {
      out += code.category().name();
      out += ": ";
      out += code.message();
    } else {
      out += "unknown error";
    }

    if (!detail.empty()) {
      out += " (";
      out += detail;
      out += ")";
    }
    return out;
  }
};

}  // namespace rediscoro

