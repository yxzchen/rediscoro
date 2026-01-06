#pragma once

#include <rediscoro/assert.hpp>
#include <rediscoro/detail/notify_event.hpp>
#include <rediscoro/detail/response_sink.hpp>
#include <rediscoro/response.hpp>

#include <iocoro/awaitable.hpp>

#include <memory>
#include <optional>
#include <variant>

namespace rediscoro::detail {

/// Typed pending response that awaits a single response value.
///
/// Implements response_sink to receive responses from pipeline.
///
/// Thread-safety model (SIMPLIFIED from original review):
/// - deliver() and deliver_error() are called ONLY from connection strand
/// - wait() is called from user's coroutine context (any executor)
/// - No cross-executor synchronization needed for deliver
/// - notify_event handles executor dispatch for wait() resumption
///
/// Why this simplification is safe:
/// - pipeline runs on connection strand
/// - pipeline is the only caller of deliver()
/// - No concurrent deliver() calls possible
/// - wait() only reads result after notification
///
/// Responsibilities:
/// - Implement response_sink interface
/// - Adapt RESP3 message to type T
/// - Store result (value or error)
/// - Provide awaitable interface via wait()
/// - Resume waiting coroutine on its original executor
///
/// Constraints:
/// - deliver() / deliver_error() can only be called once
/// - wait() can only be called once
/// - deliver() MUST be called from connection strand
template <typename T>
class pending_response : public response_sink {
public:
  pending_response() = default;

  /// Implement response_sink delivery hook.
  /// MUST be called from connection strand.
  /// MUST be called at most once.
  /// Second call is a pipeline bug (will assert in debug, ignore in release).
  auto do_deliver(resp3::message msg) -> void override;

  /// Implement response_sink error delivery hook.
  /// MUST be called from connection strand.
  /// MUST be called at most once.
  /// Second call is a pipeline bug (will assert in debug, ignore in release).
  auto do_deliver_error(resp3::error err) -> void override;

  /// Check if delivery is complete.
  [[nodiscard]] auto is_complete() const noexcept -> bool override {
    return event_.is_ready();
  }

  /// Wait for the response to complete.
  /// Returns the value or error.
  /// Can be called from any executor.
  auto wait() -> iocoro::awaitable<response_slot<T>>;

private:
  notify_event event_{};
  // No mutex needed! deliver() is single-threaded (strand-only)
  std::optional<response_slot<T>> result_{};
};

}  // namespace rediscoro::detail

#include <rediscoro/detail/impl/pending_response.ipp>
