#pragma once

#include <rediscoro/assert.hpp>
#include <rediscoro/error.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/response.hpp>

#include <cstddef>
#include <type_traits>
#include <variant>

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
/// - Makes it impossible to accidentally inline user code on the connection strand
/// - Clear separation: pipeline = scheduling, pending_response = continuation
///
/// Responsibility boundary (CRITICAL):
/// Pipeline's responsibility:
/// - NEVER call deliver() or deliver_error() more than once for the same expected reply
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

  /// Expected number of replies for this sink.
  ///
  /// For a simple single command, this is 1.
  /// For multi-reply protocols, pipeline MUST provide an appropriate sink implementation.
  [[nodiscard]] virtual auto expected_replies() const noexcept -> std::size_t {
    return 1;
  }

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
  auto deliver(resp3::message msg) -> void {
    // Structural defense: a pipeline bug must be caught immediately.
    REDISCORO_ASSERT(!is_complete() && "deliver() called on a completed sink - pipeline bug!");
    if (is_complete()) {
      return;  // Defensive in release builds
    }
    do_deliver(std::move(msg));
  }

  /// Deliver an error.
  /// Called by pipeline when parsing fails, connection closes, or other non-success events occur.
  ///
  /// Template parameters:
  /// - E: must be resp3::error or rediscoro::error (enforced at compile time)
  ///
  /// Responsibilities of implementation:
  /// 1. Store error
  /// 2. Notify waiting coroutine (on its executor)
  ///
  /// MUST NOT:
  /// - Block the calling thread
  /// - Execute user code directly
  /// - Resume coroutine inline
  template <typename E>
  auto deliver_error(E err) -> void {
    static_assert(
      std::is_same_v<E, rediscoro::error>,
      "deliver_error only accepts rediscoro::error"
    );
    
    // Structural defense: a pipeline bug must be caught immediately.
    REDISCORO_ASSERT(!is_complete() && "deliver_error() called on a completed sink - pipeline bug!");
    if (is_complete()) {
      return;  // Defensive in release builds
    }
    
    do_deliver_error(error_variant{err});
  }

  /// Fail this sink until it becomes complete.
  ///
  /// Rationale:
  /// - A request may contain multiple commands (expected_replies() > 1).
  /// - On connection close/error, the caller often needs to fail ALL remaining replies.
  ///
  /// Semantics:
  /// - Repeatedly calls deliver_error(err) until is_complete() becomes true.
  /// - Defensive: if already complete, does nothing.
  template <typename E>
  auto fail_all(E err) -> void {
    static_assert(
      std::is_same_v<E, rediscoro::error>,
      "fail_all only accepts rediscoro::error"
    );

    while (!is_complete()) {
      deliver_error(err);
    }
  }

  /// Check if delivery is complete (for diagnostics).
  [[nodiscard]] virtual auto is_complete() const noexcept -> bool = 0;

protected:
  /// Error variant for generic error delivery.
  /// Subclasses receive this and can use std::visit to dispatch to typed handlers.
  using error_variant = rediscoro::error;

  /// Implementation hooks (called only via deliver()/deliver_error()).
  virtual auto do_deliver(resp3::message msg) -> void = 0;
  virtual auto do_deliver_error(error_variant err) -> void = 0;
};

}  // namespace rediscoro::detail
