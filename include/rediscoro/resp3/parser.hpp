#pragma once

#include <rediscoro/resp3/buffer.hpp>
#include <rediscoro/resp3/raw.hpp>
#include <rediscoro/error.hpp>
#include <rediscoro/expected.hpp>

#include <span>
#include <cstddef>
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
  std::uint32_t pending_key = 0;      // for map/attr
  bool has_pending_key = false;       // for map/attr
};

/// RESP3 syntax parser: builds a zero-copy raw tree.
///
/// - incremental: call parse_one repeatedly as more data arrives
/// - zero-copy: raw_node.text is string_view into `buffer` memory (must remain stable)
/// - output: root node index into `tree().nodes`
///
/// Algorithm sketch:
/// - Maintains a stack of container frames (array/map/set/push/attribute).
/// - As scalars/containers complete, appends raw nodes into `tree_` and links children via `links`.
/// - Pending attributes are accumulated and attached to the next completed value only.
///
/// Contracts (important):
/// - pending attributes apply to the next completed value only
/// - after parse_one() returns a root, the caller must not compact/reset/reallocate the input
///   buffer until the returned raw_tree nodes are fully consumed (string_view lifetime)
/// - after consuming `tree()+root`, call reclaim() before parsing the next message
class parser {
public:
  parser() = default;

  /// Zero-copy input API (caller writes into parser-owned buffer).
  auto prepare(std::size_t min_size = 4096) -> std::span<std::byte> {
    return std::as_writable_bytes(buf_.prepare(min_size));
  }
  auto commit(std::size_t n) -> void { buf_.commit(n); }

  /// Parse exactly one RESP3 value into the internal raw_tree.
  /// On success returns root node index into tree().nodes.
  ///
  /// Returns:
  /// - success + node_index: parsing succeeded, returns root node index
  /// - error::resp3_needs_more: buffer has insufficient data, need to continue reading
  /// - error::resp3_*: protocol format error
  ///
  /// IMPORTANT: After success, you must consume the result (tree()+root) and then call reclaim()
  /// before parsing the next message, otherwise the underlying buffer may move and invalidate views.
  auto parse_one() -> expected<std::uint32_t, rediscoro::error>;

  /// Reclaim memory after consuming the latest parsed tree:
  /// - clears raw_tree
  /// - clears internal parse stack/pending attrs
  /// - compacts the internal buffer (keeps unread bytes)
  auto reclaim() -> void;

  [[nodiscard]] auto tree() noexcept -> raw_tree& { return tree_; }
  [[nodiscard]] auto tree() const noexcept -> const raw_tree& { return tree_; }

  [[nodiscard]] bool failed() const noexcept { return failed_; }

  auto reset() -> void {
    buf_.reset();
    tree_.reset();
    stack_.clear();
    failed_ = false;
    tree_ready_ = false;
    pending_attr_count_ = 0;
    pending_attr_first_ = 0;
  }

private:
  buffer buf_{};
  raw_tree tree_{};
  std::vector<frame> stack_{};
  bool failed_{false};
  bool tree_ready_{false};

  std::uint32_t pending_attr_first_{0};
  std::uint32_t pending_attr_count_{0};
};

}  // namespace rediscoro::resp3

#include <rediscoro/resp3/impl/parser.ipp>


