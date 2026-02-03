#pragma once

#include <rediscoro/resp3/kind.hpp>

namespace rediscoro::resp3 {

// Backward-compat shim: prefer `kind.hpp` + `kind` + `kind_to_prefix`.
using type3 = kind;

[[nodiscard]] constexpr auto type_to_code(type3 t) noexcept -> char { return kind_to_prefix(t); }
[[nodiscard]] constexpr auto code_to_type(char b) noexcept -> std::optional<type3> { return prefix_to_kind(b); }
[[nodiscard]] constexpr auto type_name(type3 t) noexcept -> std::string_view { return kind_name(t); }

}  // namespace rediscoro::resp3
