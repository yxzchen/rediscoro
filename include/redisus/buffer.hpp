/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <redisus/assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace redisus {

// Dynamic buffer for feeding and consuming data
// Provides both feed() for string data and prepare()/commit() for direct I/O
class buffer {
 public:
  explicit buffer(std::size_t capacity = 8192) : data_(capacity) {}

  void feed(std::string_view str) {
    ensure_writable(str.size());
    std::copy(str.begin(), str.end(), data_.begin() + write_pos_);
    write_pos_ += str.size();
  }

  std::span<char> prepare(std::size_t n) {
    ensure_writable(n);
    return std::span<char>(data_.data() + write_pos_, n);
  }

  void commit(std::size_t n) {
    REDISUS_ASSERT(write_pos_ + n <= data_.size());
    write_pos_ += n;
  }

  std::string_view view() const {
    return std::string_view(data_.data() + read_pos_, write_pos_ - read_pos_);
  }

  void consume(std::size_t n) {
    REDISUS_ASSERT(read_pos_ + n <= write_pos_);
    read_pos_ += n;
  }

  void compact() {
    if (read_pos_ == 0) return;

    std::size_t readable = write_pos_ - read_pos_;
    if (readable > 0) {
      std::memmove(data_.data(), data_.data() + read_pos_, readable);
    }

    read_pos_ = 0;
    write_pos_ = readable;

    std::size_t new_size = std::max(write_pos_ + 1024, std::size_t(8192));
    data_.resize(new_size);
    data_.shrink_to_fit();
  }

  std::size_t writable_size() const {
    return data_.size() - write_pos_;
  }

  std::size_t size() const {
    return write_pos_ - read_pos_;
  }

  bool empty() const {
    return read_pos_ == write_pos_;
  }

  std::size_t capacity() const {
    return data_.size();
  }

  void clear() {
    read_pos_ = 0;
    write_pos_ = 0;
  }

 private:
  std::vector<char> data_;
  std::size_t read_pos_ = 0;
  std::size_t write_pos_ = 0;

  void ensure_writable(std::size_t n) {
    if (writable_size() < n) {
      // Check for overflow
      if (n > std::numeric_limits<std::size_t>::max() - write_pos_) {
        throw std::length_error("Buffer size would overflow");
      }

      std::size_t needed = write_pos_ + n;
      std::size_t new_capacity = data_.size();

      // Optimize growth: double until we reach needed, or use needed if larger
      if (new_capacity == 0) new_capacity = 1;
      while (new_capacity < needed) {
        new_capacity *= 2;
      }

      data_.resize(new_capacity);
    }
  }
};

}  // namespace redisus
