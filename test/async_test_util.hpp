#pragma once

#include <xz/io/co_spawn.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/work_guard.hpp>

#include <gtest/gtest.h>

#include <exception>
#include <memory>
#include <utility>

namespace xz::redis::test_util {

inline void fail_and_stop_on_exception(std::exception_ptr eptr) {
  if (!eptr) return;
  try {
    std::rethrow_exception(eptr);
  } catch (std::exception const& e) {
    ADD_FAILURE() << "Unhandled exception in spawned coroutine: " << e.what();
  } catch (...) {
    ADD_FAILURE() << "Unhandled unknown exception in spawned coroutine";
  }
}

/// Run a coroutine on the given io_context until completion.
///
/// - Uses completion-token `co_spawn` so exceptions are captured and reported
template <class Factory>
inline void run_async(xz::io::io_context& ctx, Factory&& factory) {
  xz::io::co_spawn(
      ctx, 
      [f = std::forward<Factory>(factory)]() mutable -> xz::io::awaitable<void> { co_await f(); },
      [&](std::exception_ptr eptr) mutable { fail_and_stop_on_exception(eptr); });

  ctx.run();
}

}  // namespace xz::redis::test_util
