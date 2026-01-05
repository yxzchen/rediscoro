#pragma once

#include <span>
#include <vector>
#include <cstddef>
#include <string_view>
#include <algorithm>
#include <cstring>

namespace rediscoro::resp3 {

/// Dynamic buffer for RESP3 parsing
/// Manages a growable buffer with read/write positions
class buffer {
public:
  buffer() : buffer(4096) {}

  explicit buffer(std::size_t initial_capacity) {
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
    write_pos_ += n;
  }

  /// Get current available readable data size
  [[nodiscard]] auto size() const -> std::size_t {
    return write_pos_ - read_pos_;
  }

  /// Get readable data as a view
  [[nodiscard]] auto data() const -> std::string_view {
    return std::string_view(buffer_.data() + read_pos_, write_pos_ - read_pos_);
  }

  /// Consume n bytes from the beginning of readable data
  auto consume(std::size_t n) -> void {
    read_pos_ += n;

    // Auto-compact if we've consumed a lot and buffer is mostly empty
    if (read_pos_ > buffer_.size() / 2 && size() < buffer_.size() / 4) {
      compact();
    }
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
    auto available = buffer_.size() - write_pos_;
    if (available >= n) {
      return;
    }

    // Try compacting first
    compact();
    available = buffer_.size() - write_pos_;

    // If still not enough, resize
    if (available < n) {
      auto new_size = buffer_.size();
      while (new_size - write_pos_ < n) {
        new_size *= 2;
      }
      buffer_.resize(new_size);
    }
  }
};

}  // namespace rediscoro::resp3
