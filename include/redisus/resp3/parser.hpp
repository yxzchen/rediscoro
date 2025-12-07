/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <redisus/buffer.hpp>
#include <redisus/resp3/generator.hpp>
#include <redisus/resp3/node.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stack>
#include <string_view>

namespace redisus::resp3 {

class parser {
 public:
  static constexpr std::string_view sep = "\r\n";

  explicit parser(std::size_t buffer_capacity = 8192) : buffer_(buffer_capacity) {}

  // Feed data to the parser buffer
  void feed(std::string_view data) {
    buffer_.feed(data);
  }

  // Get writable buffer for direct I/O
  std::span<char> prepare(std::size_t n) {
    return buffer_.prepare(n);
  }

  std::error_code error() const { return ec_; }

  // Coroutine that yields parsed messages (runs forever until error)
  // Yields nullopt when needs more data, yields complete message when available
  auto parse() -> generator<std::optional<std::vector<node_view>>>;

 private:
  buffer buffer_;
  std::stack<size_t> pending_;
  std::error_code ec_;

  // Helper functions for parsing (consume buffer on successful read)
  auto read_until_separator() -> std::optional<std::string_view>;
  auto read_bulk_data(std::size_t length) -> std::optional<std::string_view>;

  void commit_elem() noexcept;
};

}  // namespace redisus::resp3
