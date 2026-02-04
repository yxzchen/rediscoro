#pragma once

#include <rediscoro/detail/connection.hpp>

#include <iocoro/bind_executor.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/when_all.hpp>
#include <iocoro/when_any.hpp>

namespace rediscoro::detail {

// -------------------- Actor loops --------------------

inline auto connection::actor_loop() -> iocoro::awaitable<void> {
  // Hard constraint (IMPORTANT):
  // - actor_loop MUST own sub-loop lifetimes (do not detached-spawn without join).

  auto parent_stop = co_await iocoro::this_coro::stop_token;
  auto ex = executor_.strand().executor();
  auto writer = iocoro::co_spawn(ex, parent_stop, iocoro::bind_executor(ex, write_loop()),
                                 iocoro::use_awaitable);
  auto reader = iocoro::co_spawn(ex, parent_stop, iocoro::bind_executor(ex, read_loop()),
                                 iocoro::use_awaitable);
  auto controller = iocoro::co_spawn(ex, parent_stop, iocoro::bind_executor(ex, control_loop()),
                                     iocoro::use_awaitable);

  (void)co_await iocoro::when_all(std::move(writer), std::move(reader), std::move(controller));

  transition_to_closed();
  co_return;
}

inline auto connection::write_loop() -> iocoro::awaitable<void> {
  auto tok = co_await iocoro::this_coro::stop_token;
  while (!tok.stop_requested() && state_ != connection_state::CLOSED) {
    if (state_ != connection_state::OPEN || !pipeline_.has_pending_write()) {
      (void)co_await write_wakeup_.async_wait();
      continue;
    }

    co_await do_write();
  }

  co_return;
}

inline auto connection::read_loop() -> iocoro::awaitable<void> {
  auto tok = co_await iocoro::this_coro::stop_token;
  while (!tok.stop_requested() && state_ != connection_state::CLOSED) {
    if (state_ != connection_state::OPEN) {
      (void)co_await read_wakeup_.async_wait();
      continue;
    }

    co_await do_read();
  }

  co_return;
}

inline auto connection::control_loop() -> iocoro::awaitable<void> {
  // Stop-aware control loop; must not write CLOSED.
  auto tok = co_await iocoro::this_coro::stop_token;
  while (!tok.stop_requested() && state_ != connection_state::CLOSED) {
    if (state_ == connection_state::FAILED) {
      if (!cfg_.reconnection.enabled) {
        // Deterministic shutdown: no reconnection.
        state_ = connection_state::CLOSING;
        stop_.request_stop();
        write_wakeup_.notify();
        read_wakeup_.notify();
        // Do not wait here: cancellation must take effect immediately.
        continue;
      }

      co_await do_reconnect();
      continue;
    }

    if (state_ == connection_state::OPEN && cfg_.request_timeout > std::chrono::milliseconds{0}) {
      if (pipeline_.has_expired()) {
        handle_error(client_errc::request_timeout);
        continue;
      }

      const auto next = pipeline_.next_deadline();
      if (next != pipeline::time_point::max()) {
        iocoro::steady_timer timer{executor_.get_io_executor()};
        timer.expires_at(next);

        auto timer_wait = timer.async_wait(iocoro::use_awaitable);
        auto wake_wait = control_wakeup_.async_wait();
        (void)co_await iocoro::when_any(std::move(timer_wait), std::move(wake_wait));
        continue;
      }
    }

    if (state_ == connection_state::CLOSING) {
      // close() or error path requested shutdown; let actor_loop join complete.
      break;
    }

    (void)co_await control_wakeup_.async_wait();
  }

  co_return;
}

}  // namespace rediscoro::detail
