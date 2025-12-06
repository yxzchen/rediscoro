/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <redisus/resp3/node.hpp>

#include <coroutine>
#include <cstdint>
#include <limits>
#include <optional>
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
    void unhandled_exception() { exception_ = std::current_exception(); }
  };

  struct iterator {
    std::coroutine_handle<promise_type> handle_;

    iterator(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

    iterator& operator++() {
      handle_.resume();
      return *this;
    }

    T& operator*() const { return handle_.promise().current_value; }

    bool operator==(std::default_sentinel_t) const { return handle_.done(); }
  };

  explicit generator(std::coroutine_handle<promise_type> handle) : handle_(handle) {}

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

  iterator begin() {
    if (handle_) {
      handle_.resume();
      if (handle_.promise().exception_) {
        std::rethrow_exception(handle_.promise().exception_);
      }
    }
    return iterator{handle_};
  }

  std::default_sentinel_t end() const noexcept { return {}; }

 private:
  std::coroutine_handle<promise_type> handle_;
};

class parser {
 public:
  static constexpr std::string_view sep = "\r\n";

  parser() { reset(); }

  // Returns true when the parser is done with the current message.
  [[nodiscard]] auto done() const noexcept -> bool;

  auto consumed() const noexcept -> std::size_t;

  // Coroutine that yields parsed nodes
  auto parse(std::string_view view, std::error_code& ec) -> generator<node_view>;

 private:
  std::string_view view_;
  std::size_t consumed_;
  std::stack<size_t> pending_;

  // Helper functions for parsing
  auto read_until_separator() -> std::optional<std::string_view>;
  auto read_bulk_data(std::size_t length) -> std::optional<std::string_view>;

  void commit_elem() noexcept;

  void reset() {
    consumed_ = 0;
    pending_ = std::stack<size_t>();
    pending_.push(1);
  }
};

}  // namespace redisus::resp3
