#pragma once

#include <iocoro/co_spawn.hpp>
#include <iocoro/expected.hpp>
#include <iocoro/io_context.hpp>
#include <iocoro/work_guard.hpp>

#include <rediscoro/src.hpp>

#include <gtest/gtest.h>

#include <exception>
#include <memory>
#include <utility>

namespace rediscoro::test_util {

inline void fail_and_stop_on_exception(std::exception_ptr eptr) {
  if (!eptr) {
    return;
  }
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
inline void run_async(iocoro::io_context& ctx, Factory&& factory) {
  auto guard = std::make_shared<iocoro::work_guard<iocoro::executor>>(ctx.get_executor());

  iocoro::co_spawn(
      ctx.get_executor(),
      [f = std::forward<Factory>(factory)]() mutable -> iocoro::awaitable<void> { co_await f(); },
      [&](iocoro::expected<void, std::exception_ptr> r) mutable {
        guard->reset();
        if (!r) {
          fail_and_stop_on_exception(r.error());
        }
      });

  ctx.run();
}

}  // namespace rediscoro::test_util
