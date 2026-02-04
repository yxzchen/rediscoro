#pragma once

#include <rediscoro/assert.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace rediscoro::resp3 {

/// Dynamic buffer for RESP3 parsing
/// Manages a growable buffer with read/write positions
class buffer {
 public:
  buffer() : buffer(4096) {}

  explicit buffer(std::size_t initial_capacity) {
    if (initial_capacity == 0) {
      initial_capacity = 1;
    }
    buffer_.resize(initial_capacity);
  }

  /// Get writable buffer space (at least min_size bytes)
  /// Returns a span that can be written to
  auto prepare(std::size_t min_size = 4096) -> std::span<char> {
    ensure_writable(min_size);
    return std::span<char>(buffer_.data() + write_pos_, buffer_.size() - write_pos_);
  }

  /// Commit n bytes that have been written to the buffer
  auto commit(std::size_t n) -> void {
    REDISCORO_ASSERT(write_pos_ <= buffer_.size());
    REDISCORO_ASSERT(n <= buffer_.size() - write_pos_);
    write_pos_ += n;
  }

  /// Get current available readable data size
  [[nodiscard]] std::size_t size() const {
    REDISCORO_ASSERT(write_pos_ >= read_pos_);
    return write_pos_ - read_pos_;
  }

  /// Get readable data as a view
  [[nodiscard]] auto data() const -> std::string_view {
    return std::string_view(buffer_.data() + read_pos_, write_pos_ - read_pos_);
  }

  /// Consume n bytes from the beginning of readable data
  auto consume(std::size_t n) -> void {
    REDISCORO_ASSERT(n <= size());
    read_pos_ += n;
  }

  /// Reset buffer to initial state (clears all data)
  auto reset() -> void {
    read_pos_ = 0;
    write_pos_ = 0;
  }

  /// Compact buffer by removing consumed data
  /// Moves unconsumed data to the beginning of the buffer
  auto compact() -> void {
    if (read_pos_ == 0) {
      return;
    }

    auto remaining = size();
    if (remaining > 0) {
      std::memmove(buffer_.data(), buffer_.data() + read_pos_, remaining);
    }

    read_pos_ = 0;
    write_pos_ = remaining;
  }

 private:
  std::vector<char> buffer_;
  std::size_t read_pos_ = 0;   // Position of next byte to read
  std::size_t write_pos_ = 0;  // Position of next byte to write

  /// Ensure buffer has at least n bytes of writable space
  auto ensure_writable(std::size_t n) -> void {
    REDISCORO_ASSERT(write_pos_ <= buffer_.size());

    if (buffer_.empty()) {
      // Prevent infinite growth loop on size==0
      buffer_.resize(std::max<std::size_t>(n, 1));
      read_pos_ = 0;
      write_pos_ = 0;
      return;
    }

    auto available = buffer_.size() - write_pos_;
    if (available >= n) {
      return;
    }

    // If still not enough, resize
    if (available < n) {
      auto new_size = buffer_.size();
      if (new_size == 0) {
        new_size = 1;
      }
      while (new_size - write_pos_ < n) {
        if (new_size > (std::numeric_limits<std::size_t>::max)() / 2) {
          new_size = write_pos_ + n;
          break;
        }
        new_size *= 2;
      }
      buffer_.resize(new_size);
    }
  }
};

}  // namespace rediscoro::resp3
