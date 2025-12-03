/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <redisus/resp3/node.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stack>
#include <string_view>

namespace redisus::resp3 {

class parser {
 public:
  using node_type = basic_node<std::string_view>;
  using result = std::optional<node_type>;

  static constexpr std::string_view sep = "\r\n";

 private:
  // Contains the length expected in the next bulk read.
  std::size_t next_length_ = std::numeric_limits<std::size_t>::max();

  // The type of the next bulk. Contains type_t::invalid if no bulk is
  // expected.
  type_t next_type_ = type_t::invalid;

  // Remaining number of aggregates.
  std::stack<size_t> remaining_;

  std::size_t consumed_;

  auto consume_impl(type_t t, std::string_view elem, std::error_code& ec) -> result;

  void commit_elem() noexcept;

 public:
  // Returns true when the parser is done with the current message.
  [[nodiscard]]
  auto done() const noexcept -> bool;

  auto consumed() const noexcept -> std::size_t;

  auto consume(std::string_view view, std::error_code& ec) noexcept -> result;
};

// Returns false if more data is needed. If true is returned the
// parser is either done or an error occured, that can be checked on
// ec.
template <class Adapter>
bool parse(parser& p, std::string_view const& msg, Adapter& adapter, std::error_code& ec) {
  while (!p.done()) {
    auto const res = p.consume(msg, ec);
    if (ec) return true;

    if (!res) return false;

    adapter.on_node(res.value(), ec);
    if (ec) return true;
  }

  return true;
}

}  // namespace redisus::resp3
