#pragma once

#include <rediscoro/adapter/error.hpp>
#include <rediscoro/resp3/type.hpp>

#include <cstdint>
#include <optional>
#include <cstddef>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rediscoro::adapter {

enum class adapter_error_kind : std::uint8_t {
  type_mismatch,
  unexpected_null,
  value_out_of_range,
  size_mismatch,
  invalid_value,
};

struct path_index {
  std::size_t index{};
};

struct path_key {
  std::string key;  // owning for stable diagnostics
};

struct path_field {
  std::string field;  // owning for stable diagnostics
};

using path_element = std::variant<path_index, path_key, path_field>;

struct adapter_error {
  adapter_error_kind kind{};
  resp3::type3 actual_type{};
  std::vector<resp3::type3> expected_types{};   // empty means "unknown / not applicable"
  std::vector<path_element> path{};
  std::optional<std::size_t> expected_size{};
  std::optional<std::size_t> got_size{};
  mutable std::optional<std::string> cached_message{};

  auto prepend_path(path_element el) -> void {
    path.insert(path.begin(), std::move(el));
    cached_message.reset();
  }

  [[nodiscard]] auto to_string() const -> const std::string& {
    if (!cached_message.has_value()) {
      cached_message = format_message(*this);
    }
    return *cached_message;
  }

  [[nodiscard]] static auto format_message(const adapter_error& e) -> std::string;
};

namespace detail {

inline auto make_type_mismatch(resp3::type3 actual, std::vector<resp3::type3> expected) -> adapter_error {
  return adapter_error{
    .kind = adapter_error_kind::type_mismatch,
    .actual_type = actual,
    .expected_types = std::move(expected),
    .path = {},
    .expected_size = std::nullopt,
    .got_size = std::nullopt,
    .cached_message = std::nullopt,
  };
}

inline auto make_unexpected_null(resp3::type3 expected) -> adapter_error {
  return adapter_error{
    .kind = adapter_error_kind::unexpected_null,
    .actual_type = resp3::type3::null,
    .expected_types = {expected},
    .path = {},
    .expected_size = std::nullopt,
    .got_size = std::nullopt,
    .cached_message = std::nullopt,
  };
}

inline auto make_value_out_of_range(resp3::type3 t) -> adapter_error {
  return adapter_error{
    .kind = adapter_error_kind::value_out_of_range,
    .actual_type = t,
    .expected_types = {t},
    .path = {},
    .expected_size = std::nullopt,
    .got_size = std::nullopt,
    .cached_message = std::nullopt,
  };
}

inline auto make_size_mismatch(resp3::type3 actual, std::size_t expected, std::size_t got) -> adapter_error {
  return adapter_error{
    .kind = adapter_error_kind::size_mismatch,
    .actual_type = actual,
    .expected_types = {actual},
    .path = {},
    .expected_size = expected,
    .got_size = got,
    .cached_message = std::nullopt,
  };
}

}  // namespace detail

inline auto adapter_error::format_message(const adapter_error& e) -> std::string {
  auto type_to_string = [](resp3::type3 t) -> std::string {
    return std::string(resp3::type_name(t));
  };

  auto path_to_string = [](const std::vector<path_element>& path) -> std::string {
    std::string out = "$";
    for (const auto& el : path) {
      std::visit([&](const auto& v) {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, path_index>) {
          out += "[" + std::to_string(v.index) + "]";
        } else if constexpr (std::is_same_v<V, path_key>) {
          out += "[\"" + v.key + "\"]";
        } else if constexpr (std::is_same_v<V, path_field>) {
          out += "." + v.field;
        }
      }, el);
    }
    return out;
  };

  const auto path = path_to_string(e.path);
  switch (e.kind) {
    case adapter_error_kind::type_mismatch: {
      if (e.expected_types.empty()) {
        return path + ": expected <?>, got " + type_to_string(e.actual_type);
      }
      if (e.expected_types.size() == 1) {
        return path + ": expected " + type_to_string(e.expected_types[0]) + ", got " + type_to_string(e.actual_type);
      }
      std::string exp = "any of: ";
      for (std::size_t i = 0; i < e.expected_types.size(); ++i) {
        if (i != 0) {
          exp += ", ";
        }
        exp += type_to_string(e.expected_types[i]);
      }
      return path + ": expected (" + exp + "), got " + type_to_string(e.actual_type);
    }
    case adapter_error_kind::unexpected_null: {
      if (e.expected_types.size() == 1) {
        return path + ": unexpected null (expected " + type_to_string(e.expected_types[0]) + ")";
      }
      return path + ": unexpected null";
    }
    case adapter_error_kind::value_out_of_range: {
      if (e.expected_types.size() == 1) {
        return path + ": value out of range for " + type_to_string(e.expected_types[0]);
      }
      return path + ": value out of range";
    }
    case adapter_error_kind::size_mismatch: {
      if (e.expected_size.has_value() && e.got_size.has_value()) {
        return path + ": size mismatch (expected " + std::to_string(*e.expected_size) + ", got " + std::to_string(*e.got_size) + ")";
      }
      return path + ": size mismatch";
    }
    case adapter_error_kind::invalid_value: {
      return path + ": invalid value";
    }
  }
  return path + ": adapter error";
}

/// Project an adapter_error into std::error_code (drops path and other structured details).
[[nodiscard]] inline auto to_error_code(const adapter_error& e) -> std::error_code {
  using rediscoro::adapter::error;

  switch (e.kind) {
    case adapter_error_kind::type_mismatch: {
      return error::type_mismatch;
    }
    case adapter_error_kind::unexpected_null: {
      return error::unexpected_null;
    }
    case adapter_error_kind::value_out_of_range: {
      return error::value_out_of_range;
    }
    case adapter_error_kind::size_mismatch: {
      return error::size_mismatch;
    }
    case adapter_error_kind::invalid_value: {
      return error::invalid_value;
    }
  }
  return error::invalid_value;
}

}  // namespace rediscoro::adapter


