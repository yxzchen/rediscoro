#pragma once

#include <rediscoro/resp3/kind.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace rediscoro::resp3 {

/// Zero-copy raw node (non-owning).
///
/// IMPORTANT: All string_views refer to the underlying input buffer memory.
/// They remain valid only while that input memory is stable (no resize/compact).
struct raw_node {
  kind type{};

  // Scalars
  std::string_view text{};  // string / error / bulk payload / number literal
  // Convention:
  // - kind::integer: i64 is the integer value
  // - bulk/container: i64 is the declared length
  // - otherwise: i64 is unspecified
  std::int64_t i64 = 0;
  // Convention:
  // - kind::double_number: f64 is the parsed value
  // - otherwise: f64 is unspecified
  double f64 = 0.0;
  bool boolean = false;

  // Composite: children are stored as indices in raw_tree::links.
  std::uint32_t first_child = 0;
  // Convention:
  // - kind::{array,set,push}: child_count == element count
  // - kind::map:              child_count == key/value node count (pairs * 2)
  // - otherwise:              child_count is 0
  std::uint32_t child_count = 0;

  // Attributes: indices in raw_tree::links, stored as key/value alternating.
  std::uint32_t first_attr = 0;
  std::uint32_t attr_count = 0;
};

/// Raw tree storage.
///
/// `nodes` stores all nodes.
/// `links` stores adjacency lists (child indices / attr indices).
struct raw_tree {
  std::vector<raw_node> nodes{};
  std::vector<std::uint32_t> links{};

  auto reset() -> void {
    nodes.clear();
    links.clear();
  }
};

}  // namespace rediscoro::resp3


