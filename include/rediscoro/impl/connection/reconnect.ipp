#pragma once

#include <rediscoro/detail/connection.hpp>

#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/when_any.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rediscoro::detail {

inline auto connection::calculate_reconnect_delay() const -> std::chrono::milliseconds {
  if (reconnect_count_ < cfg_.reconnection.immediate_attempts) {
    return std::chrono::milliseconds{0};
  }

  const auto backoff_index = reconnect_count_ - cfg_.reconnection.immediate_attempts;
  const auto base_ms = static_cast<double>(cfg_.reconnection.initial_delay.count());
  const auto factor =
    std::pow(cfg_.reconnection.backoff_factor, static_cast<double>(backoff_index));
  const auto delay_ms = base_ms * factor;

  const auto capped =
    std::min<double>(delay_ms, static_cast<double>(cfg_.reconnection.max_delay.count()));
  const auto out = static_cast<std::int64_t>(capped);
  if (out <= 0) {
    return std::chrono::milliseconds{0};
  }
  return std::chrono::milliseconds{out};
}

inline auto connection::do_reconnect() -> iocoro::awaitable<void> {
  // Precondition: called on strand.
  // State intent: FAILED -> (sleep) -> RECONNECTING -> OPEN, or exit early on close/cancel.
  auto tok = co_await iocoro::this_coro::stop_token;
  while (!tok.stop_requested() && state_ != connection_state::CLOSED) {
    // Precondition: caller enters do_reconnect() from FAILED.
    // This coroutine does not write FAILED redundantly; it only transitions:
    //   FAILED -> RECONNECTING -> (OPEN | FAILED)
    REDISCORO_ASSERT(state_ == connection_state::FAILED);
    const auto delay = calculate_reconnect_delay();

    if (delay.count() > 0) {
      // NOTE: control_wakeup_ is a counting event. It may already have pending notifications
      // (e.g. from a request_timeout path) when we enter this backoff sleep.
      // If we only wait once with when_any(timer, wake), a pending wake can skip the delay.
      //
      // Policy: treat wake-ups as "re-check conditions", but still respect the full delay
      // unless we're cancelled/closing.
      const auto deadline = pipeline::clock::now() + delay;
      iocoro::steady_timer timer{executor_.get_io_executor()};

      while (!tok.stop_requested() && state_ != connection_state::CLOSING) {
        const auto now = pipeline::clock::now();
        if (now >= deadline) {
          break;
        }

        timer.expires_after(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));

        // Wait either for the timer or for an external control signal (close/notify).
        auto timer_wait = timer.async_wait(iocoro::use_awaitable);
        auto wake_wait = control_wakeup_.async_wait();
        (void)co_await iocoro::when_any(std::move(timer_wait), std::move(wake_wait));
      }
    }

    if (tok.stop_requested() || state_ == connection_state::CLOSING) {
      co_return;
    }

    // Attempt reconnect.
    set_state(connection_state::RECONNECTING);
    auto reconnect_res = co_await do_connect();
    if (!reconnect_res) {
      // Failed attempt: transition back to FAILED and schedule next delay.
      set_state(connection_state::FAILED);
      reconnect_count_ += 1;
      auto const err = reconnect_res.error();
      emit_connection_event(connection_event{
        .kind = connection_event_kind::disconnected,
        .error = err,
      });
      continue;
    }

    // Successful do_connect() implies OPEN.
    REDISCORO_ASSERT(state_ == connection_state::OPEN);

    reconnect_count_ = 0;
    read_wakeup_.notify();
    write_wakeup_.notify();
    control_wakeup_.notify();
    co_return;
  }
}

}  // namespace rediscoro::detail
