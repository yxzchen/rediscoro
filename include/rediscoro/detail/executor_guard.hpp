#pragma once

#include <iocoro/any_executor.hpp>
#include <iocoro/io_executor.hpp>
#include <iocoro/strand.hpp>

namespace rediscoro::detail {

/// RAII wrapper for executor with strand.
///
/// Responsibilities:
/// - Ensure connection operations run on a single strand
/// - Prevent concurrent access to socket
/// - Provide stable executor reference
///
/// Critical constraints (MUST be enforced):
/// 1. NO nested co_spawn within connection
///    - worker_loop is the ONLY coroutine that accesses socket
///    - do_read/do_write/etc are subroutines, not independent coroutines
///    - Violating this breaks "single ownership of socket" invariant
///
/// 2. NO direct executor usage in connection internals
///    - All async operations use the strand executor
///    - No "optimization" by bypassing strand
///
/// 3. Strand reference is stable
///    - Can be copied safely
///    - All copies refer to the same underlying strand
///
/// Why these constraints matter:
/// - Strand serialization is the ONLY concurrency control
/// - No locks, no atomics (except in notify_event)
/// - Breaking strand guarantee = data race
///
/// Forbidden patterns:
///   // WRONG: spawning sub-coroutine
///   co_spawn(executor_.get(), async_operation(), detached);
///
///   // WRONG: bypassing strand
///   co_await socket_.async_read_some(buffer, use_awaitable);
///
/// Correct pattern:
///   // OK: direct await in worker_loop
///   co_await do_read();  // subroutine call, not spawn
class executor_guard {
public:
  explicit executor_guard(iocoro::any_executor ex)
    : strand_(iocoro::make_strand(ex)) {}

  /// Get the strand executor.
  [[nodiscard]] auto get() const noexcept -> iocoro::any_executor {
    return strand_;
  }

  /// Get the underlying io_executor (for socket construction).
  [[nodiscard]] auto get_io_executor() const -> iocoro::io_executor;

private:
  iocoro::any_executor strand_;
};

}  // namespace rediscoro::detail

#include <rediscoro/detail/impl/executor_guard.ipp>
