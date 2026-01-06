#pragma once

#include <rediscoro/resp3/message.hpp>
#include <rediscoro/resp3/error.hpp>

namespace rediscoro::detail {

/// Abstract interface for delivering responses.
///
/// Purpose (CRITICAL):
/// - Type-erases the response delivery mechanism
/// - Ensures pipeline NEVER knows about coroutines
/// - Prevents pipeline from "accidentally" resuming user code
///
/// Design principle:
/// - pipeline operates on response_sink* (abstract interface)
/// - pending_response<T> implements response_sink
/// - pipeline ONLY calls deliver() methods
/// - All coroutine resumption happens inside pending_response
///
/// Why this matters:
/// - Enforces "Pipeline never resumes coroutines" invariant at type level
/// - Makes it impossible to accidentally inline user code in worker_loop
/// - Clear separation: pipeline = scheduling, pending_response = continuation
///
/// Responsibility boundary (CRITICAL):
/// Pipeline's responsibility:
/// - NEVER call deliver() or deliver_error() more than once on the same sink
/// - Check is_complete() before attempting delivery (defensive)
/// - Remove sink from awaiting queue after delivery
///
/// Sink's responsibility:
/// - ASSERT on second deliver() call (implementation bug detection)
/// - Ignore second deliver() in release builds (defensive)
/// - Set is_complete() = true after first delivery
///
/// Why strict single-delivery:
/// - Multiple deliver() = logic error in pipeline
/// - Could cause: wrong response delivered, pending_response never completes
/// - MUST be caught during development (hence assert)
///
/// Thread safety:
/// - deliver() methods called ONLY from connection strand
/// - No cross-thread synchronization needed
/// - Implementation (pending_response) handles executor dispatch via notify_event
class response_sink {
public:
  virtual ~response_sink() = default;

  /// Deliver a successful RESP3 response.
  /// Called by pipeline when a message is received.
  ///
  /// Responsibilities of implementation:
  /// 1. Adapt message to target type T
  /// 2. Handle adaptation errors
  /// 3. Store result
  /// 4. Notify waiting coroutine (on its executor)
  ///
  /// MUST NOT:
  /// - Block the calling thread
  /// - Execute user code directly
  /// - Resume coroutine inline
  virtual auto deliver(resp3::message msg) -> void = 0;

  /// Deliver a RESP3 protocol error.
  /// Called by pipeline when parsing fails or RESP error received.
  ///
  /// Responsibilities of implementation:
  /// 1. Convert to response_error
  /// 2. Store error
  /// 3. Notify waiting coroutine (on its executor)
  ///
  /// MUST NOT:
  /// - Block the calling thread
  /// - Execute user code directly
  /// - Resume coroutine inline
  virtual auto deliver_error(resp3::error err) -> void = 0;

  /// Check if delivery is complete (for diagnostics).
  [[nodiscard]] virtual auto is_complete() const noexcept -> bool = 0;
};

}  // namespace rediscoro::detail
