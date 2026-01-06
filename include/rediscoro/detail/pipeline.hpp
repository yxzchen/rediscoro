#pragma once

#include <rediscoro/detail/response_sink.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/resp3/error.hpp>

#include <cstddef>
#include <deque>
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

  /// Enqueue a request for sending.
  /// Associates request with a response_sink for delivery.
  auto push(request req, response_sink* sink) -> void;

  /// Check if there are pending writes.
  [[nodiscard]] auto has_pending_write() const noexcept -> bool;

  /// Check if there are pending reads (responses to receive).
  [[nodiscard]] auto has_pending_read() const noexcept -> bool;

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
  auto on_error(resp3::error err) -> void;

  /// Clear all pending requests (on connection close/error).
  auto clear_all(response_error err) -> void;

  /// Get the number of pending requests (for diagnostics).
  [[nodiscard]] auto pending_count() const noexcept -> std::size_t {
    return pending_write_.size() + awaiting_read_.size();
  }

private:
  struct pending_item {
    request req;
    response_sink* sink;  // Abstract interface, no knowledge of coroutines
    std::size_t written{0};  // bytes written so far
  };

  // Requests waiting to be written to socket
  std::deque<pending_item> pending_write_{};

  // Response sinks waiting for responses (one per sent request)
  std::deque<response_sink*> awaiting_read_{};
};

}  // namespace rediscoro::detail

#include <rediscoro/detail/impl/pipeline.ipp>
