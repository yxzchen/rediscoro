#pragma once

#include <rediscoro/assert.hpp>
#include <rediscoro/error.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/response.hpp>

#include <cstddef>

namespace rediscoro::detail {

/// Abstract interface for delivering responses.
///
/// Used by the pipeline to deliver results without knowing about coroutines.
///
/// Contract (important):
/// - Called only from the connection strand.
/// - The pipeline delivers exactly `expected_replies()` replies (or errors) and never delivers
///   after completion.
/// - Deliver must not block and must not inline user code; completion/resume is handled by the
///   concrete sink (e.g. `pending_response`) on the caller's executor.
class response_sink {
public:
  virtual ~response_sink() = default;

  /// Expected number of replies for this sink.
  ///
  /// For a simple single command, this is 1.
  /// For multi-reply protocols, pipeline MUST provide an appropriate sink implementation.
  [[nodiscard]] virtual std::size_t expected_replies() const noexcept {
    return 1;
  }

  /// Deliver a successful RESP3 response.
  /// Called by pipeline when a message is received.
  /// Must not block or resume coroutines inline.
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
  /// Must not block or resume coroutines inline.
  auto deliver_error(rediscoro::error err) -> void {
    // Structural defense: a pipeline bug must be caught immediately.
    REDISCORO_ASSERT(!is_complete() && "deliver_error() called on a completed sink - pipeline bug!");
    if (is_complete()) {
      return;  // Defensive in release builds
    }

    do_deliver_error(std::move(err));
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
  auto fail_all(rediscoro::error err) -> void {
    while (!is_complete()) {
      deliver_error(err);
    }
  }

  /// Check if delivery is complete (for diagnostics).
  [[nodiscard]] virtual bool is_complete() const noexcept = 0;

protected:
  /// Implementation hooks (called only via deliver()/deliver_error()).
  virtual auto do_deliver(resp3::message msg) -> void = 0;
  virtual auto do_deliver_error(rediscoro::error err) -> void = 0;
};

}  // namespace rediscoro::detail
