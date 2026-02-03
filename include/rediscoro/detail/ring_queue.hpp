#pragma once

#include <rediscoro/assert.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace rediscoro::detail {

// A minimal cache-friendly FIFO queue backed by a growable ring buffer.
//
// Design notes:
// - Optimized for push_back/pop_front/front, typical of pipeline scheduling.
// - Not thread-safe; expected to be used on a strand.
// - Owns elements; move-only types are supported.
template <typename T, typename Alloc = std::allocator<T>>
class ring_queue {
 public:
  using value_type = T;
  using allocator_type = Alloc;
  using alloc_traits = std::allocator_traits<allocator_type>;

  ring_queue() = default;
  ring_queue(const ring_queue&) = delete;
  ring_queue& operator=(const ring_queue&) = delete;

  ring_queue(ring_queue&& other) noexcept(std::is_nothrow_move_constructible_v<allocator_type>)
      : alloc_(std::move(other.alloc_)),
        data_(other.data_),
        cap_(other.cap_),
        head_(other.head_),
        size_(other.size_) {
    other.data_ = nullptr;
    other.cap_ = 0;
    other.head_ = 0;
    other.size_ = 0;
  }

  ring_queue& operator=(ring_queue&& other) noexcept(
    alloc_traits::propagate_on_container_move_assignment::value &&
    std::is_nothrow_move_assignable_v<allocator_type>) {
    if (this == &other) {
      return *this;
    }

    if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
      clear_and_deallocate();
      alloc_ = std::move(other.alloc_);
      steal_storage(std::move(other));
      return *this;
    }

    // Non-propagating allocator:
    // - If allocators are effectively equal, we may steal storage.
    // - Otherwise fall back to element-wise move (safe for differing allocators).
    if (can_steal_storage_from(other)) {
      clear_and_deallocate();
      steal_storage(std::move(other));
      return *this;
    }

    clear();
    while (!other.empty()) {
      emplace_back(std::move(other.front()));
      other.pop_front();
    }
    return *this;
  }

  ~ring_queue() {
    clear();
    if (data_ != nullptr) {
      alloc_traits::deallocate(alloc_, data_, cap_);
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
    REDISCORO_ASSERT(cap_ > 0);
    alloc_traits::destroy(alloc_, data_ + head_);
    head_ = (head_ + 1) % cap_;
    size_ -= 1;
    if (size_ == 0) {
      head_ = 0;
    }
  }

  template <typename... Args>
  auto emplace_back(Args&&... args) -> void {
    ensure_capacity(size_ + 1);
    const auto idx = (head_ + size_) % cap_;
    alloc_traits::construct(alloc_, data_ + idx, std::forward<Args>(args)...);
    size_ += 1;
  }

  auto push_back(const T& v) -> void { emplace_back(v); }
  auto push_back(T&& v) -> void { emplace_back(std::move(v)); }

  auto clear() noexcept -> void {
    while (size_ > 0) {
      pop_front();
    }
    head_ = 0;
  }

 private:
  allocator_type alloc_{};
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

    T* new_data = alloc_traits::allocate(alloc_, new_cap);
    std::size_t constructed = 0;
    try {
      for (std::size_t i = 0; i < size_; ++i) {
        alloc_traits::construct(alloc_, new_data + i, std::move(at(i)));
        constructed += 1;
      }
    } catch (...) {
      for (std::size_t i = 0; i < constructed; ++i) {
        alloc_traits::destroy(alloc_, new_data + i);
      }
      alloc_traits::deallocate(alloc_, new_data, new_cap);
      throw;
    }

    // Destroy + deallocate old storage.
    if (data_ != nullptr) {
      for (std::size_t i = 0; i < size_; ++i) {
        alloc_traits::destroy(alloc_, data_ + ((head_ + i) % cap_));
      }
      alloc_traits::deallocate(alloc_, data_, cap_);
    }

    data_ = new_data;
    cap_ = new_cap;
    head_ = 0;
  }

  auto clear_and_deallocate() noexcept -> void {
    clear();
    if (data_ != nullptr) {
      alloc_traits::deallocate(alloc_, data_, cap_);
      data_ = nullptr;
      cap_ = 0;
      head_ = 0;
    }
  }

  auto steal_storage(ring_queue&& other) noexcept -> void {
    data_ = other.data_;
    cap_ = other.cap_;
    head_ = other.head_;
    size_ = other.size_;

    other.data_ = nullptr;
    other.cap_ = 0;
    other.head_ = 0;
    other.size_ = 0;
  }

  [[nodiscard]] auto can_steal_storage_from(const ring_queue& other) const noexcept -> bool {
    if constexpr (alloc_traits::is_always_equal::value) {
      return true;
    } else if constexpr (requires(const allocator_type& a, const allocator_type& b) { a == b; }) {
      return alloc_ == other.alloc_;
    } else {
      return false;
    }
  }
};

}  // namespace rediscoro::detail
