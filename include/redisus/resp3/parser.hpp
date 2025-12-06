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
  using result = std::optional<node_view>;

  static constexpr std::string_view sep = "\r\n";

 private:
  type3 bulk_type_ = type3::invalid;
  std::size_t bulk_length_;

  // Remaining number of aggregates.
  std::stack<size_t> pending_;

  std::size_t consumed_;

  auto consume_impl(type3 t, std::string_view elem, std::error_code& ec) -> result;

  void commit_elem() noexcept;

  void reset() {
    bulk_type_ = type3::invalid;
    consumed_ = 0;

    pending_ = std::stack<size_t>();
    pending_.push(1);
  }

 public:
  parser() {
    reset();
  }

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
