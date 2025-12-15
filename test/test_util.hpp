#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/io/io_context.hpp>

#include <exception>
#include <string>
#include <system_error>
#include <utility>

namespace xz::redis::test_util {

struct run_result {
  std::error_code ec{};
  std::string what{};
};

/// Runs a coroutine on an io_context and captures any exception thrown.
///
/// This exists because `co_spawn(..., use_detached)` swallows exceptions. In tests, we
/// want failures to be observable and to shut down the io_context cleanly.
template <class Factory, class Cleanup>
auto run_io(xz::io::io_context& ctx, Factory&& factory, Cleanup&& cleanup) -> run_result {
  auto res = std::make_shared<run_result>();

  xz::io::co_spawn(
      ctx,
      [res,
       f = std::forward<Factory>(factory),
       c = std::forward<Cleanup>(cleanup)]() mutable -> xz::io::awaitable<void> {
        try {
          co_await f();
        } catch (std::system_error const& e) {
          res->ec = e.code();
          res->what = e.what();
        } catch (std::exception const& e) {
          res->what = e.what();
        } catch (...) {
          res->what = "unknown exception";
        }

        // Ensure shutdown happens on the io_context thread.
        try {
          c();
        } catch (...) {
          // Don't let cleanup failures mask the original exception.
        }
      }(),
      xz::io::use_detached);

  ctx.run();
  return *res;
}

}  // namespace xz::redis::test_util


