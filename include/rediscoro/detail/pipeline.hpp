#pragma once

#include <rediscoro/detail/response_sink.hpp>
#include <rediscoro/error.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/resp3/message.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace rediscoro::detail {

/// Request-response pipeline scheduler.
///
/// Responsibilities:
/// - Maintain FIFO ordering of requests
/// - Track pending writes and reads
/// - Dispatch RESP3 messages to response_sink
///
/// NOT responsible for:
/// - IO operations
/// - Executor management
/// - Resuming coroutines (response_sink handles this)
/// - Knowing about coroutine types (works only with abstract interface)
///
/// Type-level guarantee:
/// - pipeline operates ONLY on response_sink* (abstract interface)
/// - pipeline CANNOT access pending_response<T> or coroutine handles
/// - This prevents accidental inline resumption of user code
///
/// Thread safety:
/// - All methods MUST be called from the connection's strand
/// - No internal synchronization (relies on strand serialization)
class pipeline {
public:
  pipeline() = default;

  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;

  /// Enqueue a request for sending.
  /// Associates request with a response_sink for delivery.
  ///
  /// Reply-count contract (IMPORTANT):
  /// - A request may represent a pipeline of multiple commands (request.reply_count() > 1).
  /// - pipeline MUST NOT deliver more than sink->expected_replies() replies into a sink.
  /// - For a fixed-size sink (pending_response<Ts...>), req.reply_count() MUST equal sizeof...(Ts)
  ///   (enforced at connection::enqueue<Ts...> boundary).
  auto push(request req, std::shared_ptr<response_sink> sink) -> void;

  /// Enqueue a request with a timeout deadline.
  ///
  /// deadline == time_point::max() means "no timeout".
  auto push(request req, std::shared_ptr<response_sink> sink, time_point deadline) -> void;

  /// Check if there are pending writes.
  [[nodiscard]] bool has_pending_write() const noexcept;

  /// Check if there are pending reads (responses to receive).
  [[nodiscard]] bool has_pending_read() const noexcept;

  /// Get the next buffer to write.
  /// Precondition: has_pending_write() == true
  [[nodiscard]] auto next_write_buffer() -> std::string_view;

  /// Mark N bytes as written.
  /// When a request is fully written, it moves to the awaiting queue.
  auto on_write_done(std::size_t n) -> void;

  /// Dispatch a received RESP3 message to the next pending response.
  /// Precondition: has_pending_read() == true
  auto on_message(resp3::message msg) -> void;

  /// Dispatch a RESP3 parse error to the next pending response.
  /// Precondition: has_pending_read() == true
  auto on_error(error err) -> void;

  /// Clear all pending requests (on connection close/error).
  auto clear_all(rediscoro::error err) -> void;

  /// Earliest deadline among all pending requests.
  /// Returns time_point::max() if there is no deadline.
  [[nodiscard]] auto next_deadline() const noexcept -> time_point;

  /// True if the earliest pending request has reached its deadline.
  [[nodiscard]] bool has_expired() const noexcept;

  /// Get the number of pending requests (for diagnostics).
  [[nodiscard]] std::size_t pending_count() const noexcept {
    return pending_write_.size() + awaiting_read_.size();
  }

private:
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

  struct pending_item {
    request req;
    std::shared_ptr<response_sink> sink;  // Abstract interface, no knowledge of coroutines
    std::size_t written{0};  // bytes written so far
    time_point deadline{time_point::max()};
  };

  struct awaiting_item {
    std::shared_ptr<response_sink> sink;  // Abstract interface
    time_point deadline{time_point::max()};
  };

  // Requests waiting to be written to socket
  ring_queue<pending_item> pending_write_{};

  // Response sinks waiting for responses (one per sent request)
  ring_queue<awaiting_item> awaiting_read_{};
};

}  // namespace rediscoro::detail

#include <rediscoro/impl/pipeline.ipp>
