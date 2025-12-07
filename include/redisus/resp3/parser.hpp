/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <redisus/buffer.hpp>
#include <redisus/resp3/node.hpp>

#include <coroutine>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stack>
#include <string_view>

namespace redisus::resp3 {

// Generator for C++20 coroutines
template <typename T>
class generator {
 public:
  struct promise_type {
    T current_value;
    std::exception_ptr exception_;

    generator get_return_object() {
      return generator{std::coroutine_handle<promise_type>::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    std::suspend_always yield_value(T value) noexcept {
      current_value = std::move(value);
      return {};
    }

    void return_void() noexcept {}
    void unhandled_exception() noexcept {
      // TODO: Add logging when logger is available
      exception_ = std::current_exception();
    }
  };

  using handle_type = std::coroutine_handle<promise_type>;

  explicit generator(handle_type handle) : handle_(handle) {}

  ~generator() {
    if (handle_) handle_.destroy();
  }

  // Move only
  generator(const generator&) = delete;
  generator& operator=(const generator&) = delete;
  generator(generator&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  generator& operator=(generator&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  bool next() {
    if (!handle_ || handle_.done()) return false;
    handle_.resume();
    if (handle_.promise().exception_) {
      return false;
    }
    return true;
  }

  T& value() { return handle_.promise().current_value; }

 private:
  handle_type handle_;
};

class parser {
 public:
  static constexpr std::string_view sep = "\r\n";

  explicit parser(std::size_t buffer_capacity = 8192) : buffer_(buffer_capacity) {
    pending_.push(1);
  }

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

  // Parser state for resumption
  enum class state {
    read_header,
    read_bulk_data,
  };

  state state_ = state::read_header;
  std::size_t pending_bulk_length_ = 0;
  type3 pending_type_ = type3::invalid;

  // Helper functions for parsing (consume buffer on successful read)
  auto read_until_separator() -> std::optional<std::string_view>;
  auto read_bulk_data(std::size_t length) -> std::optional<std::string_view>;

  void commit_elem() noexcept;
};

}  // namespace redisus::resp3
