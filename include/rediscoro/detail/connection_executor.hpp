#pragma once

#include <iocoro/io_executor.hpp>
#include <iocoro/strand.hpp>

namespace rediscoro::detail {

/// RAII wrapper for binding the connection to a strand executor.
///
/// Responsibilities:
/// - Ensure connection operations run on a single strand
/// - Prevent concurrent access to socket
/// - Provide stable executor reference
///
/// Critical constraints (MUST be enforced):
/// 1. All connection internals run on ONE strand
///    - Multiple coroutines are allowed (read_loop / write_loop / control_loop)
///    - They MUST all be spawned/bound onto this SAME strand executor
///    - All state_, pipeline_, and socket_ *lifecycle* mutations are serialized by the strand
///
///    Note: socket IO is full-duplex:
///    - One async_read_some and one async_write_some may be in-flight concurrently
///    - But you MUST NOT start two concurrent reads or two concurrent writes
///      (enforced by per-direction in-flight flags inside connection)
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
///   // WRONG: spawning onto non-strand executor (breaks serialization)
///   co_spawn(io_executor, async_operation(), detached);
///
///   // WRONG: bypassing strand
///   co_await socket_.async_read_some(buffer, use_awaitable);
///
/// Correct patterns:
///   // OK: spawn coroutines onto the same strand
///   co_spawn(executor_.strand().executor(), read_loop(), detached);
///   co_spawn(executor_.strand().executor(), write_loop(), detached);
///
///   // OK: direct await inside any strand-bound coroutine
///   co_await socket_.async_read_some(buf, iocoro::bind_executor(executor_.strand().executor(), use_awaitable));
class connection_executor {
public:
  explicit connection_executor(iocoro::io_executor ex)
    : io_executor_(ex)
    , strand_(iocoro::make_strand(iocoro::any_executor{ex})) {}

  /// Strand executor façade.
  ///
  /// Design goal: reduce accidental misuse inside connection internals.
  /// - This is NOT implicitly convertible to iocoro::any_executor.
  /// - If you really need the raw executor, you must call .executor() explicitly.
  class strand_facade {
  public:
    explicit strand_facade(iocoro::any_executor ex) : ex_(std::move(ex)) {}

    [[nodiscard]] auto executor() const noexcept -> iocoro::any_executor {
      return ex_;
    }

  private:
    iocoro::any_executor ex_;
  };

  /// Get the strand executor façade.
  [[nodiscard]] auto strand() const noexcept -> strand_facade {
    return strand_facade{strand_};
  }

  /// Get the underlying io_executor (for socket construction).
  [[nodiscard]] auto get_io_executor() const -> iocoro::io_executor {
    return io_executor_;
  }

private:
  iocoro::io_executor io_executor_{};
  iocoro::any_executor strand_;
};

}  // namespace rediscoro::detail
