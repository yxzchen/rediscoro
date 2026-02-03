#pragma once

#include <rediscoro/error.hpp>

#include <system_error>
#include <string>
#include <string_view>

namespace rediscoro {

/// A compact error object with:
/// - a stable error_code (domain + value)
/// - an optional detail string (human-oriented; may include context and underlying cause)
/// - an optional underlying std::error_code (program-oriented; no nesting)
struct error_info {
  std::error_code code{};
  std::string detail{};
  std::error_code cause_ec{};

  error_info() = default;

  explicit error_info(std::error_code c) : code(c) {}

  error_info(std::error_code c, std::string d) : code(c), detail(std::move(d)) {}

  template <typename Errc>
    requires requires(Errc e) { std::error_code{e}; }
  explicit error_info(Errc e) : code(std::error_code{e}) {}

  template <typename Errc>
    requires requires(Errc e) { std::error_code{e}; }
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

  auto set_cause(std::error_code ec) -> error_info& {
    cause_ec = ec;
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
    if (!detail.empty()) {
      return out;
    }

    // If there's no detail, we can still include a concise cause if present.
    if (cause_ec) {
      out += " (cause=";
      out += cause_ec.category().name();
      out += ": ";
      out += cause_ec.message();
      out += ")";
    }

    return out;
  }
};

}  // namespace rediscoro

