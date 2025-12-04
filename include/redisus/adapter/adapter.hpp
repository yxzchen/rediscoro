/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <redisus/error.hpp>
#include <redisus/resp3/node.hpp>
#include <redisus/resp3/type.hpp>

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace redisus::adapter {

// Convert RESP3 node to string
inline void from_node(std::string& result, resp3::node_view const& node, std::error_code& ec) {
  if (resp3::is_aggregate(node.data_type)) {
    ec = error::expects_resp3_simple_type;
    return;
  }

  if (node.data_type == resp3::type_t::null) {
    result.clear();
    return;
  }

  result = node.value();
}

// Convert RESP3 node to string_view
inline void from_node(std::string_view& result, resp3::node_view const& node, std::error_code& ec) {
  if (resp3::is_aggregate(node.data_type)) {
    ec = error::expects_resp3_simple_type;
    return;
  }

  if (node.data_type == resp3::type_t::null) {
    result = std::string_view{};
    return;
  }

  result = node.value();
}

// Convert RESP3 node to integral type
template <class T>
auto from_node(T& result, resp3::node_view const& node, std::error_code& ec)
  -> std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, void> {
  if (resp3::is_aggregate(node.data_type)) {
    ec = error::expects_resp3_simple_type;
    return;
  }

  auto const sv = node.value();
  auto const res = std::from_chars(sv.data(), sv.data() + sv.size(), result);
  if (res.ec != std::errc{}) {
    ec = error::not_a_number;
  }
}

// Convert RESP3 node to double
inline void from_node(double& result, resp3::node_view const& node, std::error_code& ec) {
  if (resp3::is_aggregate(node.data_type)) {
    ec = error::expects_resp3_simple_type;
    return;
  }

  auto const sv = node.value();
  try {
    result = std::stod(std::string(sv));
  } catch (...) {
    ec = error::not_a_number;
  }
}

// Convert RESP3 node to bool
inline void from_node(bool& result, resp3::node_view const& node, std::error_code& ec) {
  if (resp3::is_aggregate(node.data_type)) {
    ec = error::expects_resp3_simple_type;
    return;
  }

  auto const sv = node.value();
  if (sv.size() == 1) {
    if (sv[0] == 't' || sv[0] == '1') {
      result = true;
      return;
    } else if (sv[0] == 'f' || sv[0] == '0') {
      result = false;
      return;
    }
  }

  ec = error::expects_resp3_simple_type;
}

}  // namespace redisus::adapter
