#pragma once

#include <rediscoro/error.hpp>
#include <rediscoro/resp3/kind.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace rediscoro::adapter {

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

struct error {  // NOLINT(clang-analyzer-core.uninitialized.Assign)
  rediscoro::adapter_errc kind{};
  resp3::kind actual_type{};
  std::vector<resp3::kind> expected_types{};  // empty means "unknown / not applicable"
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

  [[nodiscard]] static auto format_message(const error& e) -> std::string;
};

namespace detail {

inline auto make_type_mismatch(resp3::kind actual, std::vector<resp3::kind> expected) -> error {
  return error{
    .kind = rediscoro::adapter_errc::type_mismatch,
    .actual_type = actual,
    .expected_types = std::move(expected),
    .path = {},
    .expected_size = std::nullopt,
    .got_size = std::nullopt,
    .cached_message = std::nullopt,
  };
}

inline auto make_unexpected_null(std::vector<resp3::kind> expected) -> error {
  return error{
    .kind = rediscoro::adapter_errc::unexpected_null,
    .actual_type = resp3::kind::null,
    .expected_types = std::move(expected),
    .path = {},
    .expected_size = std::nullopt,
    .got_size = std::nullopt,
    .cached_message = std::nullopt,
  };
}

inline auto make_value_out_of_range(resp3::kind k) -> error {
  return error{
    .kind = rediscoro::adapter_errc::value_out_of_range,
    .actual_type = k,
    .expected_types = {k},
    .path = {},
    .expected_size = std::nullopt,
    .got_size = std::nullopt,
    .cached_message = std::nullopt,
  };
}

inline auto make_size_mismatch(resp3::kind actual, std::size_t expected, std::size_t got) -> error {
  return error{
    .kind = rediscoro::adapter_errc::size_mismatch,
    .actual_type = actual,
    .expected_types = {},
    .path = {},
    .expected_size = expected,
    .got_size = got,
    .cached_message = std::nullopt,
  };
}

}  // namespace detail

inline auto error::format_message(const error& e) -> std::string {
  auto type_to_string = [](resp3::kind k) -> std::string {
    return std::string(resp3::kind_name(k));
  };

  auto path_to_string = [](const std::vector<path_element>& path) -> std::string {
    std::string out = "$";
    for (const auto& el : path) {
      std::visit(
        [&](const auto& v) {
          using V = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<V, path_index>) {
            out += "[" + std::to_string(v.index) + "]";
          } else if constexpr (std::is_same_v<V, path_key>) {
            out += "[\"" + v.key + "\"]";
          } else if constexpr (std::is_same_v<V, path_field>) {
            out += "." + v.field;
          }
        },
        el);
    }
    return out;
  };

  const auto path = path_to_string(e.path);
  switch (e.kind) {
    case rediscoro::adapter_errc::type_mismatch: {
      if (e.expected_types.empty()) {
        return path + ": expected <?>, got " + type_to_string(e.actual_type);
      }
      if (e.expected_types.size() == 1) {
        return path + ": expected " + type_to_string(e.expected_types[0]) + ", got " +
               type_to_string(e.actual_type);
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
    case rediscoro::adapter_errc::unexpected_null: {
      if (e.expected_types.size() == 1) {
        return path + ": unexpected null (expected " + type_to_string(e.expected_types[0]) + ")";
      }
      if (!e.expected_types.empty()) {
        std::string exp = "any of: ";
        for (std::size_t i = 0; i < e.expected_types.size(); ++i) {
          if (i != 0) {
            exp += ", ";
          }
          exp += type_to_string(e.expected_types[i]);
        }
        return path + ": unexpected null (expected " + exp + ")";
      }
      return path + ": unexpected null";
    }
    case rediscoro::adapter_errc::value_out_of_range: {
      if (e.expected_types.size() == 1) {
        return path + ": value out of range for " + type_to_string(e.expected_types[0]);
      }
      return path + ": value out of range";
    }
    case rediscoro::adapter_errc::size_mismatch: {
      if (e.expected_size.has_value() && e.got_size.has_value()) {
        return path + ": size mismatch (expected " + std::to_string(*e.expected_size) + ", got " +
               std::to_string(*e.got_size) + ")";
      }
      return path + ": size mismatch";
    }
  }
  return path + ": adapter error";
}

}  // namespace rediscoro::adapter
