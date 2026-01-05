#pragma once

#include <rediscoro/resp3/buffer.hpp>
#include <rediscoro/resp3/error.hpp>
#include <rediscoro/resp3/raw.hpp>
#include <rediscoro/expected.hpp>

#include <cstdint>
#include <vector>

namespace rediscoro::resp3 {

enum class frame_kind : std::uint8_t {
  value,
  array,
  map_key,
  map_value,
  attribute,
};

struct frame {
  frame_kind kind{frame_kind::value};
  type3 container_type{type3::null};

  // For map/attribute: expected & produced count PAIRS (key/value), not nodes.
  std::int64_t expected = 0;          // container len (pairs for map/attr)
  std::uint32_t produced = 0;         // produced children (or pairs)
  std::uint32_t node_index = 0;       // container node (for array/map/set/push)
  std::uint32_t first_link = 0;       // start offset in raw_tree::links
  std::uint32_t pending_key = 0;      // for map/attr
  bool has_pending_key = false;       // for map/attr
};

/// RESP3 syntax parser: builds a zero-copy raw tree.
///
/// - incremental: call parse_one repeatedly as more data arrives
/// - zero-copy: raw_node.text is string_view into `buffer` memory (must remain stable)
/// - output: root node index into `tree().nodes`
///
/// Contracts (important):
/// - pending attributes apply to the next completed value only
/// - after parse_one() returns a root, the caller must not compact/reset/reallocate the input
///   buffer until the returned raw_tree nodes are fully consumed (string_view lifetime)
class parser {
public:
  parser() = default;

  auto parse_one(buffer& buf) -> expected<std::uint32_t, error>;

  [[nodiscard]] auto tree() noexcept -> raw_tree& { return tree_; }
  [[nodiscard]] auto tree() const noexcept -> const raw_tree& { return tree_; }

  [[nodiscard]] auto failed() const noexcept -> bool { return failed_; }

  auto reset() -> void {
    tree_.reset();
    stack_.clear();
    failed_ = false;
    pending_attr_count_ = 0;
    pending_attr_first_ = 0;
  }

private:
  raw_tree tree_{};
  std::vector<frame> stack_{};
  bool failed_{false};

  std::uint32_t pending_attr_first_{0};
  std::uint32_t pending_attr_count_{0};
};

}  // namespace rediscoro::resp3

#include <rediscoro/resp3/impl/parser.ipp>


