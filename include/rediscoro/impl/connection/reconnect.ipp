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
  const auto min_delay_ms = static_cast<double>(cfg_.reconnection.initial_delay.count());
  const auto max_delay_ms = static_cast<double>(cfg_.reconnection.max_delay.count());
  const auto base_ms = min_delay_ms;
  const auto factor =
    std::pow(cfg_.reconnection.backoff_factor, static_cast<double>(backoff_index));
  const auto delay_ms = base_ms * factor;
  auto bounded_ms = std::clamp(delay_ms, min_delay_ms, max_delay_ms);

  if (bounded_ms > 0.0 && cfg_.reconnection.jitter_ratio > 0.0) {
    // Simple xorshift64* for jitter sampling (no allocation/exception path).
    static thread_local std::uint64_t jitter_state = 0x4d595df4d0f33173ULL;
    jitter_state ^= (static_cast<std::uint64_t>(reconnect_count_) << 1);
    jitter_state ^= (generation_ << 17);
    jitter_state ^= (jitter_state >> 12);
    jitter_state ^= (jitter_state << 25);
    jitter_state ^= (jitter_state >> 27);

    const double unit = static_cast<double>((jitter_state * 2685821657736338717ULL) >> 11) *
                        (1.0 / 9007199254740992.0);
    const double scale =
      (1.0 - cfg_.reconnection.jitter_ratio) + (2.0 * cfg_.reconnection.jitter_ratio * unit);
    bounded_ms *= scale;
    bounded_ms = std::clamp(bounded_ms, min_delay_ms, max_delay_ms);
  }

  const auto out = static_cast<std::int64_t>(std::llround(bounded_ms));
  if (out <= 0) {
    return std::chrono::milliseconds{0};
  }
  return std::chrono::milliseconds{out};
}

inline auto connection::do_reconnect() -> iocoro::awaitable<void> {
  // Precondition: called on strand.
  // State intent: FAILED -> (sleep) -> RECONNECTING -> OPEN, or exit early on close/cancel.
  auto tok = co_await iocoro::this_coro::stop_token;
  REDISCORO_LOG_DEBUG("connection.reconnect.loop_start state={}", to_string(state_));
  while (!tok.stop_requested() && state_ != connection_state::CLOSED) {
    // Precondition: caller enters do_reconnect() from FAILED.
    // This coroutine does not write FAILED redundantly; it only transitions:
    //   FAILED -> RECONNECTING -> (OPEN | FAILED)
    REDISCORO_ASSERT(state_ == connection_state::FAILED);
    const auto delay = calculate_reconnect_delay();
    REDISCORO_LOG_INFO("connection.reconnect.attempt index={} delay_ms={} generation={}",
                       reconnect_count_ + 1, delay.count(), generation_);

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
        REDISCORO_LOG_DEBUG("connection.reconnect.backoff_wakeup");
      }
    }

    if (tok.stop_requested() || state_ == connection_state::CLOSING) {
      REDISCORO_LOG_INFO("connection.reconnect.cancelled state={}", to_string(state_));
      co_return;
    }

    // Attempt reconnect.
    REDISCORO_LOG_INFO("connection.state_transition reason=reconnect_attempt from={} to={}",
                       to_string(connection_state::FAILED),
                       to_string(connection_state::RECONNECTING));
    set_state(connection_state::RECONNECTING);
    auto reconnect_res = co_await do_connect();
    if (!reconnect_res) {
      // Failed attempt: transition back to FAILED and schedule next delay.
      REDISCORO_LOG_WARNING("connection.reconnect.failed err_code={} err_msg={} detail={}",
                            reconnect_res.error().code.value(),
                            reconnect_res.error().code.message(), reconnect_res.error().detail);
      REDISCORO_LOG_INFO("connection.state_transition reason=reconnect_failed from={} to={}",
                         to_string(connection_state::RECONNECTING),
                         to_string(connection_state::FAILED));
      set_state(connection_state::FAILED);
      reconnect_count_ += 1;
      continue;
    }

    // Successful do_connect() implies OPEN.
    REDISCORO_ASSERT(state_ == connection_state::OPEN);

    reconnect_count_ = 0;
    REDISCORO_LOG_INFO("connection.reconnect.succeeded generation={}", generation_);
    read_wakeup_.notify();
    write_wakeup_.notify();
    control_wakeup_.notify();
    co_return;
  }
}

}  // namespace rediscoro::detail
