/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace redisus {

// Dynamic buffer for feeding and consuming data
// Provides both feed() for string data and prepare()/commit() for direct I/O
class buffer {
 public:
  explicit buffer(std::size_t capacity = 8192) : data_(capacity) {}

  // Feed string data to the buffer
  void feed(std::string_view str) {
    ensure_writable(str.size());
    std::copy(str.begin(), str.end(), data_.begin() + write_pos_);
    write_pos_ += str.size();
  }

  // Prepare writable buffer for direct I/O (returns span to write into)
  std::span<char> prepare(std::size_t n) {
    ensure_writable(n);
    return std::span<char>(data_.data() + write_pos_, n);
  }

  // Commit n bytes that were written to the buffer from prepare()
  void commit(std::size_t n) {
    write_pos_ += n;
  }

  // Get readable view of all buffered data
  std::string_view view() const {
    return std::string_view(data_.data() + read_pos_, write_pos_ - read_pos_);
  }

  // Consume n bytes (advance read position)
  // Does not auto-compact to keep string_views valid
  void consume(std::size_t n) {
    read_pos_ += n;
  }

  // Compact buffer by moving data to the beginning
  void compact() {
    if (read_pos_ == 0) return;

    std::size_t readable = write_pos_ - read_pos_;
    if (readable > 0) {
      std::memmove(data_.data(), data_.data() + read_pos_, readable);
    }

    read_pos_ = 0;
    write_pos_ = readable;
  }

  // Get available write space
  std::size_t writable_size() const {
    return data_.size() - write_pos_;
  }

  // Get readable size
  std::size_t size() const {
    return write_pos_ - read_pos_;
  }

  // Check if buffer is empty
  bool empty() const {
    return read_pos_ == write_pos_;
  }

  // Get buffer capacity
  std::size_t capacity() const {
    return data_.size();
  }

  // Clear all data
  void clear() {
    read_pos_ = 0;
    write_pos_ = 0;
  }

 private:
  std::vector<char> data_;
  std::size_t read_pos_ = 0;   // Start of readable data
  std::size_t write_pos_ = 0;  // End of readable data / start of writable area

  // Ensure we have at least n bytes of writable space
  void ensure_writable(std::size_t n) {
    // Compact first to reclaim consumed space
    if (read_pos_ > 0) {
      compact();
    }

    // If still not enough, grow the buffer
    if (writable_size() < n) {
      std::size_t new_capacity = data_.size();
      while (new_capacity - write_pos_ < n) {
        new_capacity *= 2;
      }
      data_.resize(new_capacity);
    }
  }
};

}  // namespace redisus
