#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace rediscoro::resp3 {

enum class raw_type : std::uint8_t {
  simple_string,
  simple_error,
  integer,
  double_type,
  boolean,
  big_number,
  null,
  bulk_string,
  bulk_error,
  verbatim_string,
  array,
  map,
  set,
  push,
  attribute,
};

/// Zero-copy raw node (non-owning).
///
/// IMPORTANT: All string_views refer to the underlying input buffer memory.
/// They remain valid only while that input memory is stable (no resize/compact).
struct raw_node {
  raw_type type{};

  // Scalars
  std::string_view text{};  // string / error / bulk payload / number literal
  std::int64_t i64 = 0;     // integer / len (for bulk/container)
  double f64 = 0.0;
  bool boolean = false;

  // Composite: children are stored as indices in raw_tree::links.
  std::uint32_t first_child = 0;
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


