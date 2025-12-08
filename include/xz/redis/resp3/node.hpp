#pragma once

#include <cstddef>
#include <variant>
#include <xz/redis/resp3/type.hpp>

namespace xz::redis::resp3 {

/** @brief A node in the response tree.
 *
 *  RESP3 can contain recursive data structures, like a map of sets of
 *  vectors. This class is called a node
 *  because it can be seen as the element of the response tree.
 *
 *  The node uses std::variant to store either an aggregate size (for
 *  aggregate types like arrays, maps, sets) or a value (for simple types
 *  like strings, numbers, etc.), providing memory efficiency.
 *
 *  @tparam String A `std::string`-like type.
 */
template <class String>
struct basic_node {
  /// The RESP3 type of the data in this node.
  type3 data_type = type3::invalid;

  /// The data: either aggregate size or value.
  std::variant<std::size_t, String> data;

  /** @brief Returns the aggregate size.
   *  @note Only valid when data_type is an aggregate type.
   *  @return The number of elements in the aggregate.
   */
  auto aggregate_size() const -> std::size_t { return std::get<std::size_t>(data); }

  /** @brief Returns the value.
   *  @note Only valid when data_type is a simple type.
   *  @return Reference to the value string.
   */
  auto value() const -> String const& { return std::get<String>(data); }

  /** @brief Checks if this node holds an aggregate.
   *  @return True if the node contains an aggregate size, false if it contains a value.
   */
  auto is_aggregate_node() const -> bool { return std::holds_alternative<std::size_t>(data); }
};

using node = basic_node<std::string>;
using node_view = basic_node<std::string_view>;

using msg_view = std::vector<node_view>;

inline auto to_owning_node(node_view const& view) -> node {
  node result;
  result.data_type = view.data_type;

  if (view.is_aggregate_node()) {
    result.data = view.aggregate_size();
  } else {
    result.data = std::string(view.value());
  }

  return result;
}

inline auto to_owning_nodes(std::vector<node_view> const& views) -> std::vector<node> {
  std::vector<node> result;
  result.reserve(views.size());
  for (auto const& view : views) {
    result.push_back(to_owning_node(view));
  }
  return result;
}

}  // namespace xz::redis::resp3
