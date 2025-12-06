/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <redisus/resp3/type.hpp>
#include <variant>
#include <cstddef>

namespace redisus::resp3 {

/** @brief A node in the response tree.
 *
 *  RESP3 can contain recursive data structures, like a map of sets of
 *  vectors. This class is called a node
 *  because it can be seen as the element of the response tree. It
 *  is a template so that users can use it with any string type, like
 *  `std::string` or `boost::static_string`.
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
  auto aggregate_size() const -> std::size_t {
    return std::get<std::size_t>(data);
  }

  /** @brief Returns the value.
   *  @note Only valid when data_type is a simple type.
   *  @return Reference to the value string.
   */
  auto value() const -> String const& {
    return std::get<String>(data);
  }

  /** @brief Checks if this node holds an aggregate.
   *  @return True if the node contains an aggregate size, false if it contains a value.
   */
  auto is_aggregate_node() const -> bool {
    return std::holds_alternative<std::size_t>(data);
  }
};

/// A node in the response tree that owns its data.
using node = basic_node<std::string>;

/// A node in the response tree that does not own its data.
using node_view = basic_node<std::string_view>;

}  // namespace redisus::resp3
