#pragma once

#include <rediscoro/assert.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>

namespace rediscoro::detail {

// A minimal cache-friendly FIFO queue backed by a growable ring buffer.
//
// Design notes:
// - Optimized for push_back/pop_front/front, typical of pipeline scheduling.
// - Not thread-safe; expected to be used on a strand.
// - Owns elements; move-only types are supported.
template <typename T>
class ring_queue {
public:
  ring_queue() = default;
  ring_queue(const ring_queue&) = delete;
  ring_queue& operator=(const ring_queue&) = delete;

  ring_queue(ring_queue&& other) noexcept
    : data_(other.data_), cap_(other.cap_), head_(other.head_), size_(other.size_) {
    other.data_ = nullptr;
    other.cap_ = 0;
    other.head_ = 0;
    other.size_ = 0;
  }

  ring_queue& operator=(ring_queue&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    clear();
    if (data_ != nullptr) {
      alloc_.deallocate(data_, cap_);
    }

    data_ = other.data_;
    cap_ = other.cap_;
    head_ = other.head_;
    size_ = other.size_;

    other.data_ = nullptr;
    other.cap_ = 0;
    other.head_ = 0;
    other.size_ = 0;
    return *this;
  }

  ~ring_queue() {
    clear();
    if (data_ != nullptr) {
      alloc_.deallocate(data_, cap_);
    }
  }

  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  [[nodiscard]] auto front() -> T& {
    REDISCORO_ASSERT(size_ > 0);
    return data_[head_];
  }

  [[nodiscard]] auto front() const -> const T& {
    REDISCORO_ASSERT(size_ > 0);
    return data_[head_];
  }

  auto pop_front() -> void {
    REDISCORO_ASSERT(size_ > 0);
    std::allocator_traits<std::allocator<T>>::destroy(alloc_, data_ + head_);
    head_ = (head_ + 1) % cap_;
    size_ -= 1;
    if (size_ == 0) {
      head_ = 0;
    }
  }

  auto push_back(T v) -> void {
    ensure_capacity(size_ + 1);
    const auto idx = (head_ + size_) % cap_;
    std::allocator_traits<std::allocator<T>>::construct(alloc_, data_ + idx, std::move(v));
    size_ += 1;
  }

  auto clear() noexcept -> void {
    while (size_ > 0) {
      pop_front();
    }
    head_ = 0;
  }

private:
  std::allocator<T> alloc_{};
  T* data_{nullptr};
  std::size_t cap_{0};
  std::size_t head_{0};
  std::size_t size_{0};

  [[nodiscard]] auto at(std::size_t i) -> T& {
    REDISCORO_ASSERT(i < size_);
    const auto idx = (head_ + i) % cap_;
    return data_[idx];
  }

  auto ensure_capacity(std::size_t need) -> void {
    if (cap_ >= need) {
      return;
    }

    auto new_cap = std::max<std::size_t>(8, cap_);
    while (new_cap < need) {
      new_cap *= 2;
    }

    T* new_data = alloc_.allocate(new_cap);
    std::size_t constructed = 0;
    try {
      for (std::size_t i = 0; i < size_; ++i) {
        std::allocator_traits<std::allocator<T>>::construct(
          alloc_, new_data + i, std::move(at(i)));
        constructed += 1;
      }
    } catch (...) {
      for (std::size_t i = 0; i < constructed; ++i) {
        std::allocator_traits<std::allocator<T>>::destroy(alloc_, new_data + i);
      }
      alloc_.deallocate(new_data, new_cap);
      throw;
    }

    // Destroy + deallocate old storage.
    if (data_ != nullptr) {
      for (std::size_t i = 0; i < size_; ++i) {
        std::allocator_traits<std::allocator<T>>::destroy(alloc_, data_ + ((head_ + i) % cap_));
      }
      alloc_.deallocate(data_, cap_);
    }

    data_ = new_data;
    cap_ = new_cap;
    head_ = 0;
  }
};

}  // namespace rediscoro::detail

