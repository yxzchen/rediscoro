#pragma once

#include <rediscoro/assert.hpp>
#include <rediscoro/error_info.hpp>
#include <rediscoro/logger.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/tracing.hpp>

#include <chrono>
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
  [[nodiscard]] virtual std::size_t expected_replies() const noexcept { return 1; }

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
  auto deliver_error(error_info err) -> void {
    // Structural defense: a pipeline bug must be caught immediately.
    REDISCORO_ASSERT(!is_complete() &&
                     "deliver_error() called on a completed sink - pipeline bug!");
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
  auto fail_all(error_info err) -> void {
    while (!is_complete()) {
      deliver_error(err);
    }
  }

  /// Check if delivery is complete (for diagnostics).
  [[nodiscard]] virtual bool is_complete() const noexcept = 0;

  auto set_trace_context(request_trace_hooks hooks, request_trace_info info,
                         std::chrono::steady_clock::time_point start) noexcept -> void {
    trace_hooks_ = hooks;
    trace_info_ = info;
    trace_start_ = start;
    trace_enabled_ = hooks.enabled();
    trace_finished_ = false;
  }

  [[nodiscard]] auto has_trace_context() const noexcept -> bool { return trace_enabled_; }

 protected:
  struct trace_summary {
    std::size_t ok_count{0};
    std::size_t error_count{0};
    std::error_code primary_error{};
    std::string_view primary_error_detail{};
  };

  /// Implementation hooks (called only via deliver()/deliver_error()).
  virtual auto do_deliver(resp3::message msg) -> void = 0;
  virtual auto do_deliver_error(error_info err) -> void = 0;

  [[nodiscard]] auto trace_hooks() const noexcept -> request_trace_hooks const& {
    return trace_hooks_;
  }
  [[nodiscard]] auto trace_info() const noexcept -> request_trace_info const& {
    return trace_info_;
  }
  [[nodiscard]] auto trace_start() const noexcept -> std::chrono::steady_clock::time_point {
    return trace_start_;
  }

  [[nodiscard]] auto try_mark_trace_finished() noexcept -> bool {
    if (!trace_enabled_) {
      return false;
    }
    if (trace_finished_) {
      return false;
    }
    trace_finished_ = true;
    return true;
  }

  auto emit_trace_finish(trace_summary const& summary) noexcept -> void {
    if (!has_trace_context()) {
      return;
    }
    auto const& hooks = trace_hooks();
    if (hooks.on_finish == nullptr) {
      return;
    }
    if (!try_mark_trace_finished()) {
      return;
    }

    request_trace_finish evt{
      .info = trace_info(),
      .duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - trace_start()),
      .ok_count = summary.ok_count,
      .error_count = summary.error_count,
      .primary_error = summary.primary_error,
      .primary_error_detail = summary.primary_error_detail,
    };

    // Callbacks are user-provided: do not allow exceptions to escape.
    try {
      hooks.on_finish(hooks.user_data, evt);
    } catch (...) {
      REDISCORO_LOG_WARNING("connection.trace.on_finish_threw request_id={} kind={}",
                            trace_info().id, to_string(trace_info().kind));
    }
  }

 private:
  request_trace_hooks trace_hooks_{};
  request_trace_info trace_info_{};
  std::chrono::steady_clock::time_point trace_start_{};
  bool trace_enabled_{false};
  bool trace_finished_{false};
};

}  // namespace rediscoro::detail
