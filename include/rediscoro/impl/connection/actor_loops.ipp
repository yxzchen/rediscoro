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
  REDISCORO_LOG_DEBUG("actor loop start");
  auto writer = iocoro::co_spawn(ex, parent_stop, iocoro::bind_executor(ex, write_loop()),
                                 iocoro::use_awaitable);
  auto reader = iocoro::co_spawn(ex, parent_stop, iocoro::bind_executor(ex, read_loop()),
                                 iocoro::use_awaitable);
  auto controller = iocoro::co_spawn(ex, parent_stop, iocoro::bind_executor(ex, control_loop()),
                                     iocoro::use_awaitable);

  (void)co_await iocoro::when_all(std::move(writer), std::move(reader), std::move(controller));

  REDISCORO_LOG_DEBUG("actor loop end");
  transition_to_closed();
  co_return;
}

inline auto connection::write_loop() -> iocoro::awaitable<void> {
  auto tok = co_await iocoro::this_coro::stop_token;
  REDISCORO_LOG_DEBUG("write loop start");
  while (!tok.stop_requested() && state_ != connection_state::CLOSED) {
    if (state_ != connection_state::OPEN || !pipeline_.has_pending_write()) {
      (void)co_await write_wakeup_.async_wait();
      continue;
    }

    co_await do_write();
  }

  REDISCORO_LOG_DEBUG("write loop stop");
  co_return;
}

inline auto connection::read_loop() -> iocoro::awaitable<void> {
  auto tok = co_await iocoro::this_coro::stop_token;
  REDISCORO_LOG_DEBUG("read loop start");
  while (!tok.stop_requested() && state_ != connection_state::CLOSED) {
    if (state_ != connection_state::OPEN) {
      (void)co_await read_wakeup_.async_wait();
      continue;
    }

    co_await do_read();
  }

  REDISCORO_LOG_DEBUG("read loop stop");
  co_return;
}

inline auto connection::control_loop() -> iocoro::awaitable<void> {
  // Stop-aware control loop; must not write CLOSED.
  auto tok = co_await iocoro::this_coro::stop_token;
  REDISCORO_LOG_DEBUG("control loop start");
  while (!tok.stop_requested() && state_ != connection_state::CLOSED) {
    if (state_ == connection_state::FAILED) {
      if (!cfg_.reconnection.enabled) {
        // Deterministic shutdown: no reconnection.
        REDISCORO_LOG_INFO("state transition: reason=reconnect_disabled from={} to={}",
                           to_string(connection_state::FAILED),
                           to_string(connection_state::CLOSING));
        set_state(connection_state::CLOSING);
        stop_.request_stop();
        write_wakeup_.notify();
        read_wakeup_.notify();
        // Do not wait here: cancellation must take effect immediately.
        continue;
      }

      co_await do_reconnect();
      continue;
    }

    if (state_ == connection_state::OPEN && cfg_.request_timeout.has_value()) {
      if (pipeline_.has_expired()) {
        REDISCORO_LOG_DEBUG("request timeout deadline reached");
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
        REDISCORO_LOG_DEBUG("request timeout wait woke up (timer or control signal)");
        continue;
      }
    }

    if (state_ == connection_state::CLOSING) {
      // close() or error path requested shutdown; let actor_loop join complete.
      break;
    }

    (void)co_await control_wakeup_.async_wait();
  }

  REDISCORO_LOG_DEBUG("control loop stop");
  co_return;
}

}  // namespace rediscoro::detail
