#pragma once

#include <rediscoro/error.hpp>
#include <rediscoro/expected.hpp>
#include <rediscoro/resp3/buffer.hpp>
#include <rediscoro/resp3/raw.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace rediscoro::resp3 {

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
  /// - success + root_index: parsing succeeded, returns root node index
  /// - success + nullopt: buffer has insufficient data, need to continue reading
  /// - protocol_errc: protocol format error
  ///
  /// IMPORTANT: After success, you must consume the result (tree()+root) and then call reclaim()
  /// before parsing the next message, otherwise the underlying buffer may move and invalidate views.
  auto parse_one() -> expected<std::optional<std::uint32_t>, rediscoro::protocol_errc>;

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
    pending_attrs_.reset();
  }

 private:
  enum class frame_kind : std::uint8_t {
    value,
    array,      // array/set/push elements
    map_key,    // map key
    map_value,  // map value
    attribute,  // attribute key/value pairs
  };

  struct frame {
    frame_kind state{frame_kind::value};
    kind container_type{kind::null};

    // For map/attribute: expected & produced count PAIRS (key/value), not nodes.
    std::int64_t expected = 0;      // container len (pairs for map/attr)
    std::uint32_t produced = 0;     // produced children (or pairs)
    std::uint32_t node_index = 0;   // container node (for array/map/set/push)
    std::uint32_t pending_key = 0;  // for map/attr
    bool has_pending_key = false;   // for map/attr
  };

  struct length_header {
    std::int64_t length{};
    std::size_t header_bytes{};
  };

  struct pending_attributes {
    std::uint32_t first{0};
    std::uint32_t count{0};

    [[nodiscard]] auto empty() const noexcept -> bool { return count == 0; }

    auto reset() noexcept -> void {
      first = 0;
      count = 0;
    }

    auto push(raw_tree& tree, std::uint32_t kv_node) -> void {
      if (count == 0) {
        first = static_cast<std::uint32_t>(tree.links.size());
      }
      tree.links.push_back(kv_node);
      count += 1;
    }

    auto attach(raw_tree& tree, std::uint32_t node_idx) -> void {
      if (count == 0) {
        return;
      }
      auto& n = tree.nodes.at(node_idx);
      n.first_attr = first;
      n.attr_count = count;
      reset();
    }
  };

  enum class step : std::uint8_t {
    continue_parsing = 0,
    produced,  // index is valid
  };

  struct step_index {
    step step{step::continue_parsing};
    std::uint32_t index{0};
  };

  buffer buf_{};
  raw_tree tree_{};
  std::vector<frame> stack_{};
  bool failed_{false};
  bool tree_ready_{false};

  pending_attributes pending_attrs_{};

  [[nodiscard]] auto parse_length_after_type(std::string_view data) const
    -> expected<std::optional<length_header>, rediscoro::protocol_errc>;
  [[nodiscard]] auto parse_value() -> expected<std::optional<step_index>, rediscoro::protocol_errc>;
  [[nodiscard]] auto start_container(frame& current, kind t, std::int64_t len)
    -> expected<step_index, rediscoro::protocol_errc>;
  [[nodiscard]] auto start_attribute(std::int64_t len)
    -> expected<std::monostate, rediscoro::protocol_errc>;
  [[nodiscard]] auto attach_to_parent(std::uint32_t child_idx)
    -> expected<step_index, rediscoro::protocol_errc>;
};

}  // namespace rediscoro::resp3

#include <rediscoro/resp3/impl/parser.ipp>
