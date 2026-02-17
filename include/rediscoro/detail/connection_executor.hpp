#pragma once

#include <rediscoro/detail/internal_header_access.hpp>

#include <iocoro/any_io_executor.hpp>
#include <iocoro/strand.hpp>

namespace rediscoro::detail {

/// RAII wrapper for binding the connection to a strand executor.
///
/// Purpose: make “everything runs on the connection strand” hard to violate.
///
/// Contract:
/// - All connection internals (state machine + pipeline + socket lifecycle) are serialized on a
///   single strand executor.
/// - Socket IO is full-duplex: at most one in-flight read and one in-flight write are allowed
///   concurrently (the connection enforces the per-direction rule).
/// - Connection code must not bypass the strand by awaiting/spawning on other executors.
/// - The strand handle is stable/copyable (copies refer to the same strand).
class connection_executor {
 public:
  explicit connection_executor(iocoro::any_io_executor ex)
      : io_executor_(ex), strand_(iocoro::make_strand(iocoro::any_executor{ex})) {}

  /// Strand executor façade.
  ///
  /// Design goal: reduce accidental misuse inside connection internals.
  /// - This is NOT implicitly convertible to iocoro::any_executor.
  /// - If you really need the raw executor, you must call .executor() explicitly.
  class strand_facade {
   public:
    explicit strand_facade(iocoro::any_executor ex) : ex_(std::move(ex)) {}

    [[nodiscard]] auto executor() const noexcept -> iocoro::any_executor { return ex_; }

   private:
    iocoro::any_executor ex_;
  };

  /// Get the strand executor façade.
  [[nodiscard]] auto strand() const noexcept -> strand_facade { return strand_facade{strand_}; }

  /// Get the underlying io_executor (for socket construction).
  [[nodiscard]] auto get_io_executor() const -> iocoro::any_io_executor { return io_executor_; }

 private:
  iocoro::any_io_executor io_executor_{};
  iocoro::any_executor strand_;
};

}  // namespace rediscoro::detail
