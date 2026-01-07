#pragma once

#include <rediscoro/detail/connection.hpp>
#include <rediscoro/ignore.hpp>
#include <rediscoro/resp3/builder.hpp>

#include <iocoro/bind_executor.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/io/connect.hpp>
#include <iocoro/ip/resolver.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/with_timeout.hpp>
#include <iocoro/when_all.hpp>
#include <iocoro/when_any.hpp>

#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <system_error>

namespace rediscoro::detail {

inline connection::connection(iocoro::io_executor ex, config cfg)
  : cfg_(std::move(cfg))
  , executor_(ex)
  , socket_(executor_.get_io_executor()) {
}

inline auto connection::run_actor() -> void {
  REDISCORO_ASSERT(!actor_awaitable_.has_value() && "run_actor() called while actor is running");

  auto ex = executor_.strand().any_executor();
  actor_awaitable_.emplace(
    iocoro::co_spawn(ex, iocoro::bind_executor(ex, actor_loop()), iocoro::use_awaitable)
  );
}

inline auto connection::connect() -> iocoro::awaitable<std::error_code> {
  // CRITICAL: All state mutations MUST occur on the connection's strand to prevent
  // data races with the background loops.
  co_await iocoro::this_coro::switch_to(executor_.strand().any_executor());

  // Single-await rule for actor_awaitable_ (CRITICAL):
  // - iocoro::co_spawn(use_awaitable) supports only one awaiter.
  // - We enforce: ONLY close() awaits actor_awaitable_.
  // - connect() MUST NOT co_await actor_awaitable_ directly.
  // - On connect failure, connect() should co_await close() to perform cleanup and join.

  if (state_ == connection_state::OPEN) {
    co_return std::error_code{};
  }

  if (state_ == connection_state::CONNECTING) {
    co_return error::already_in_progress;
  }

  if (state_ == connection_state::CLOSED) {
    // Retry support: reset lifecycle state.
    state_ = connection_state::INIT;
    last_error_.reset();
    reconnect_count_ = 0;
    cancel_.reset();
  }

  if (cancel_.is_cancelled()) {
    co_return error::operation_aborted;
  }

  if (!actor_awaitable_.has_value()) {
    run_actor();
  }

  state_ = connection_state::CONNECTING;

  // Attempt connection. do_connect() returns error on failure.
  auto connect_ec = co_await do_connect();

  if (cancel_.is_cancelled()) {
    // close() won; unify cleanup via close().
    co_await close();
    co_return error::operation_aborted;
  }

  if (state_ == connection_state::OPEN) {
    // Wake IO loops that might be waiting for the OPEN transition.
    read_wakeup_.notify();
    write_wakeup_.notify();
    control_wakeup_.notify();
    co_return std::error_code{};
  }

  // Initial connect failure MUST NOT enter FAILED state (FAILED is reserved for runtime errors).
  // Cleanup must be unified via close() (single awaiter of actor_awaitable_).
  // Note: connect_error is NOT stored in last_error_ (user gets it directly from connect()).
  auto ec = connect_ec;
  if (!ec) {
    ec = error::connect_failed;
  }
  co_await close();
  co_return ec;
}

inline auto connection::close() -> iocoro::awaitable<void> {
  co_await iocoro::this_coro::switch_to(executor_.strand().any_executor());

  if (state_ == connection_state::CLOSED) {
    co_return;
  }

  // Phase-1: determinism-first shutdown.
  cancel_.request_cancel();
  state_ = connection_state::CLOSING;

  // Fail all pending work deterministically.
  pipeline_.clear_all(error::connection_closed);

  // Close socket immediately.
  if (socket_.is_open()) {
    socket_.close();
  }

  // Wake loops / actor.
  write_wakeup_.notify();
  read_wakeup_.notify();
  control_wakeup_.notify();

  if (actor_awaitable_.has_value()) {
    // Critical: actor_awaitable_ can only be awaited once.
    co_await std::move(*actor_awaitable_);
    actor_awaitable_.reset();
  }

  REDISCORO_ASSERT(state_ == connection_state::CLOSED);
  co_return;
}

inline auto connection::enqueue_impl(request req, response_sink* sink) -> void {
  REDISCORO_ASSERT(sink != nullptr);

  // State gating: reject early if not ready.
  switch (state_) {
    case connection_state::INIT:
    case connection_state::CONNECTING: {
      sink->fail_all(error::not_connected);
      return;
    }
    case connection_state::FAILED:
    case connection_state::RECONNECTING: {
      sink->fail_all(error::connection_lost);
      return;
    }
    case connection_state::CLOSING:
    case connection_state::CLOSED: {
      sink->fail_all(error::connection_closed);
      return;
    }
    case connection_state::OPEN: {
      break;
    }
  }

  pipeline::time_point deadline = pipeline::time_point::max();
  if (cfg_.request_timeout > std::chrono::milliseconds{0}) {
    deadline = pipeline::clock::now() + cfg_.request_timeout;
  }
  pipeline_.push(std::move(req), sink, deadline);
  write_wakeup_.notify();
  control_wakeup_.notify();  // request_timeout scheduling / wake control_loop when first request arrives
}

inline auto connection::actor_loop() -> iocoro::awaitable<void> {
  // Minimal actor (step-2):
  // - Own write_loop lifetime so close() can use actor_awaitable_ as the shutdown barrier.
  // - Step-3: own read_loop lifetime as well (joined with write_loop).
  // - Step-3: own control_loop lifetime as well (control_wakeup_ must not be a ghost).
  //
  // Hard constraint (IMPORTANT):
  // - actor_loop MUST own sub-loop lifetimes (do not detached-spawn without join).

  auto ex = executor_.strand().any_executor();
  auto writer = iocoro::co_spawn(ex, iocoro::bind_executor(ex, write_loop()), iocoro::use_awaitable);
  auto reader = iocoro::co_spawn(ex, iocoro::bind_executor(ex, read_loop()), iocoro::use_awaitable);
  auto controller = iocoro::co_spawn(ex, iocoro::bind_executor(ex, control_loop()), iocoro::use_awaitable);

  (void)co_await iocoro::when_all(std::move(writer), std::move(reader), std::move(controller));

  // CRITICAL: Only transition_to_closed() is allowed to write state_ = CLOSED.
  transition_to_closed();
  co_return;
}

inline auto connection::write_loop() -> iocoro::awaitable<void> {
  while (!cancel_.is_cancelled() && state_ != connection_state::CLOSED) {
    if (state_ != connection_state::OPEN || !pipeline_.has_pending_write()) {
      co_await write_wakeup_.wait();
      continue;
    }

    co_await do_write();
  }

  co_return;
}

inline auto connection::read_loop() -> iocoro::awaitable<void> {
  while (!cancel_.is_cancelled() && state_ != connection_state::CLOSED) {
    if (state_ != connection_state::OPEN) {
      co_await read_wakeup_.wait();
      continue;
    }

    co_await do_read();
  }

  co_return;
}

inline auto connection::control_loop() -> iocoro::awaitable<void> {
  // Control loop (step-6):
  // - Centralized state transitions and reconnection policy.
  //
  // IMPORTANT constraints:
  // - Must NOT write CLOSED (only transition_to_closed()).
  // - Must be cancel-aware so close() can interrupt promptly.
  while (!cancel_.is_cancelled() && state_ != connection_state::CLOSED) {
    if (state_ == connection_state::FAILED) {
      if (!cfg_.reconnection.enabled) {
        // Deterministic shutdown: no reconnection.
        state_ = connection_state::CLOSING;
        cancel_.request_cancel();
        write_wakeup_.notify();
        read_wakeup_.notify();
        // Do not wait here: cancellation must take effect immediately.
        continue;
      }

      co_await do_reconnect();
      continue;
    }

    if (state_ == connection_state::OPEN && cfg_.request_timeout > std::chrono::milliseconds{0}) {
      auto now = pipeline::clock::now();
      if (pipeline_.has_expired(now)) {
        handle_error(error::request_timeout);
        continue;
      }

      const auto next = pipeline_.next_deadline();
      if (next != pipeline::time_point::max()) {
        iocoro::steady_timer timer{socket_.get_executor()};
        timer.expires_at(next);

        auto timer_wait = timer.async_wait(iocoro::use_awaitable);
        auto wake_wait = control_wakeup_.wait();
        (void)co_await iocoro::when_any(std::move(timer_wait), std::move(wake_wait));
        continue;
      }
    }

    if (state_ == connection_state::CLOSING) {
      // close() or error path requested shutdown; let actor_loop join complete.
      break;
    }

    co_await control_wakeup_.wait();
  }

  co_return;
}

inline auto connection::calculate_reconnect_delay() const -> std::chrono::milliseconds {
  if (reconnect_count_ < cfg_.reconnection.immediate_attempts) {
    return std::chrono::milliseconds{0};
  }

  const auto backoff_index = reconnect_count_ - cfg_.reconnection.immediate_attempts;
  const auto base_ms = static_cast<double>(cfg_.reconnection.initial_delay.count());
  const auto factor = std::pow(cfg_.reconnection.backoff_factor, static_cast<double>(backoff_index));
  const auto delay_ms = base_ms * factor;

  const auto capped = std::min<double>(delay_ms, static_cast<double>(cfg_.reconnection.max_delay.count()));
  const auto out = static_cast<std::int64_t>(capped);
  if (out <= 0) {
    return std::chrono::milliseconds{0};
  }
  return std::chrono::milliseconds{out};
}

inline auto connection::do_reconnect() -> iocoro::awaitable<void> {
  // Precondition: called on strand.
  // State intent: FAILED -> (sleep) -> RECONNECTING -> OPEN, or exit early on close/cancel.
  while (!cancel_.is_cancelled() && state_ != connection_state::CLOSED) {
    // Precondition: caller enters do_reconnect() from FAILED.
    // This coroutine does not write FAILED redundantly; it only transitions:
    //   FAILED -> RECONNECTING -> (OPEN | FAILED)
    REDISCORO_ASSERT(state_ == connection_state::FAILED);
    const auto delay = calculate_reconnect_delay();

    if (delay.count() > 0) {
      iocoro::steady_timer timer{socket_.get_executor()};
      timer.expires_after(delay);

      // Wait either for the timer or for an external control signal (close/notify).
      auto timer_wait = timer.async_wait(iocoro::use_awaitable);
      auto wake_wait = control_wakeup_.wait();
      (void)co_await iocoro::when_any(std::move(timer_wait), std::move(wake_wait));
    }

    if (cancel_.is_cancelled() || state_ == connection_state::CLOSING) {
      co_return;
    }

    // Attempt reconnect.
    state_ = connection_state::RECONNECTING;
    auto reconnect_ec = co_await do_connect();

    if (state_ == connection_state::OPEN) {
      reconnect_count_ = 0;
      last_error_.reset();  // Clear error on successful reconnect
      read_wakeup_.notify();
      write_wakeup_.notify();
      control_wakeup_.notify();
      co_return;
    }

    if (cancel_.is_cancelled() || state_ == connection_state::CLOSING) {
      co_return;
    }

    // Failed attempt: transition back to FAILED and schedule next delay.
    // Store the reconnection error for diagnostics (user cannot obtain it directly).
    state_ = connection_state::FAILED;
    if (!reconnect_ec) {
      last_error_ = error::connect_failed;
    } else if (reconnect_ec.category() == rediscoro::detail::category()) {
      last_error_ = static_cast<error>(reconnect_ec.value());
    } else {
      last_error_ = error::connect_failed;
    }
    reconnect_count_ += 1;
  }
}

inline auto connection::do_connect() -> iocoro::awaitable<std::error_code> {
  if (cancel_.is_cancelled()) {
    co_return error::operation_aborted;
  }

  // Defensive: ensure parser state is clean at the start of a handshake.
  // This prevents accidental carry-over between retries or reconnect attempts.
  parser_.reset();

  // Resolve:
  // - iocoro resolver always runs getaddrinfo on a background thread_pool and resumes on this
  //   coroutine's executor (our strand).
  // - It does NOT support cancellation; timeout/close will only stop waiting (the resolve may
  //   still complete in the background and be ignored).
  iocoro::ip::tcp::resolver resolver{};
  auto res = co_await iocoro::with_timeout_detached(
    socket_.get_executor(),
    resolver.async_resolve(cfg_.host, std::to_string(cfg_.port)),
    cfg_.resolve_timeout
  );
  if (!res.has_value()) {
    if (res.error() == iocoro::error::timed_out) {
      co_return error::resolve_timeout;
    } else {
      co_return error::resolve_failed;
    }
  }
  if (res->empty()) {
    co_return error::resolve_failed;
  }

  if (cancel_.is_cancelled()) {
    co_return error::operation_aborted;
  }

  // TCP connect with timeout (try endpoints in order).
  std::error_code connect_ec{};
  for (auto const& ep : *res) {
    // IMPORTANT: after a failed connect attempt, the socket may be left in a platform-dependent
    // error state. Always close before trying the next endpoint.
    if (socket_.is_open()) {
      socket_.close();
    }

    connect_ec = co_await iocoro::io::async_connect_timeout(socket_, ep, cfg_.connect_timeout);
    if (!connect_ec) {
      break;
    }
  }

  if (connect_ec) {
    // Map timeout/cancel vs generic connect failure.
    if (connect_ec == iocoro::error::timed_out) {
      co_return error::connect_timeout;
    } else if (connect_ec == iocoro::error::operation_aborted) {
      co_return error::operation_aborted;
    } else {
      co_return error::connect_failed;
    }
  }

  if (cancel_.is_cancelled()) {
    co_return error::operation_aborted;
  }

  // Build handshake request (pipeline of commands).
  request req{};
  req.push("HELLO", "3");
  if (!cfg_.password.empty()) {
    if (!cfg_.username.empty()) {
      req.push("AUTH", cfg_.username, cfg_.password);
    } else {
      req.push("AUTH", cfg_.password);
    }
  }
  if (cfg_.database != 0) {
    req.push("SELECT", cfg_.database);
  }
  if (!cfg_.client_name.empty()) {
    req.push("CLIENT", "SETNAME", cfg_.client_name);
  }

  auto slot = std::make_shared<pending_dynamic_response<ignore_t>>(req.reply_count());
  pipeline_.push(std::move(req), slot.get());

  // Drive handshake IO directly (read/write loops are gated on OPEN so they will not interfere).
  auto handshake_ec = co_await iocoro::with_timeout(
    socket_.get_executor(),
    [&](iocoro::cancellation_token tok) -> iocoro::awaitable<std::error_code> {
      while (!cancel_.is_cancelled() && !slot->is_complete()) {
        // Write all pending handshake bytes.
        while (pipeline_.has_pending_write()) {
          auto view = pipeline_.next_write_buffer();
          auto buf = std::span<std::byte const>{
            reinterpret_cast<std::byte const*>(view.data()),
            view.size()
          };
          auto w = co_await socket_.async_write_some(buf, tok);
          if (!w.has_value()) {
            co_return w.error();
          }
          pipeline_.on_write_done(*w);
        }

        // Read until we can parse at least one value or hit timeout/cancel.
        auto writable = parser_.prepare();
        auto rbuf = std::span<std::byte>{
          reinterpret_cast<std::byte*>(writable.data()),
          writable.size()
        };
        auto r = co_await socket_.async_read_some(rbuf, tok);
        if (!r.has_value()) {
          co_return r.error();
        }
        if (*r == 0) {
          co_return error::connection_reset;
        }
        parser_.commit(*r);

        for (;;) {
          auto parsed = parser_.parse_one();
          if (!parsed.has_value()) {
            if (parsed.error() == error::resp3_needs_more) {
              break;
            }
            if (pipeline_.has_pending_read()) {
              pipeline_.on_error(parsed.error());
            }
            co_return parsed.error();
          }

          if (!pipeline_.has_pending_read()) {
            co_return error::unsolicited_message;
          }

          auto msg = resp3::build_message(parser_.tree(), *parsed);
          pipeline_.on_message(std::move(msg));
          parser_.reclaim();

          if (slot->is_complete()) {
            break;
          }
        }
      }

      co_return std::error_code{};
    },
    cfg_.connect_timeout);

  // NOTE (current limitation):
  // - connect() lifecycle cancellation (cancel_) is not wired into iocoro::cancellation_token.
  // - If cancel_ is requested while an async_* operation is in-flight, we only observe it after
  //   that operation resumes (or times out). This is acceptable for now; future work may bridge
  //   cancel_ into an iocoro cancellation_source to provide prompt abort semantics.

  if (cancel_.is_cancelled() || handshake_ec == iocoro::error::operation_aborted) {
    // Handshake aborted (close/cancel won). Fail the internal handshake sink deterministically.
    pipeline_.clear_all(error::operation_aborted);
    co_return error::operation_aborted;
  }

  if (handshake_ec == iocoro::error::timed_out) {
    pipeline_.clear_all(error::handshake_timeout);
    co_return error::handshake_timeout;
  }

  if (handshake_ec) {
    // Fail the internal handshake sink with a meaningful error; connect() returns the error directly.
    // Unsolicited server messages during handshake are treated as unsupported feature for now.
    if (handshake_ec == error::unsolicited_message) {
      pipeline_.clear_all(error::handshake_failed);
      co_return error::handshake_failed;
    } else if (handshake_ec == error::connection_reset) {
      // Preserve the semantic error: peer closed/reset during handshake.
      pipeline_.clear_all(error::connection_reset);
      co_return error::connection_reset;
    } else {
      // TCP is already connected here; any remaining error is either:
      // - a RESP3 protocol parsing error, or
      // - an IO failure during handshake (system error_code).
      //
      // With unified error codes, preserve RESP3 protocol errors for better diagnostics.
      // Check if the error is a rediscoro::error by attempting conversion.
      if (handshake_ec.category() == rediscoro::detail::category()) {
        // It's a rediscoro::error - preserve it (could be RESP3 protocol error).
        auto e = static_cast<error>(handshake_ec.value());
        pipeline_.clear_all(e);
        co_return e;
      } else {
        // System/IO error during handshake - convert to handshake_failed.
        pipeline_.clear_all(error::handshake_failed);
        co_return error::handshake_failed;
      }
    }
  }

  // Validate all handshake replies: any error => handshake_failed.
  //
  // Defensive: handshake_ec == {} implies slot should be complete (loop condition). Keep this
  // check to avoid future hangs if the handshake loop logic changes.
  if (!slot->is_complete()) {
    pipeline_.clear_all(error::connection_closed);
    co_return error::handshake_failed;
  }
  auto results = co_await slot->wait();
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (!results[i].has_value()) {
      pipeline_.clear_all(error::connection_closed);
      co_return error::handshake_failed;
    }
  }

  // Handshake succeeded.
  state_ = connection_state::OPEN;
  reconnect_count_ = 0;

  // Defensive: ensure parser buffer/state is clean when handing over to runtime loops.
  parser_.reset();
  co_return std::error_code{};
}

inline auto connection::do_read() -> iocoro::awaitable<void> {
  if (state_ != connection_state::OPEN) {
    co_return;
  }

  struct in_flight_guard {
    bool& flag;
    explicit in_flight_guard(bool& f) : flag(f) {
      REDISCORO_ASSERT(!flag, "concurrent read detected");
      flag = true;
    }
    ~in_flight_guard() { flag = false; }
  };

  in_flight_guard guard{read_in_flight_};

  // Socket-driven read: perform one read operation (may parse multiple messages from the buffer).
  // This allows detecting peer close even when no pending_read exists.
  auto writable = parser_.prepare();
  auto buf = std::span<std::byte>{
    reinterpret_cast<std::byte*>(writable.data()),
    writable.size()
  };

  auto r = co_await socket_.async_read_some(buf);
  if (!r.has_value()) {
    // Socket IO error - treat as connection lost
    handle_error(error::connection_lost);
    co_return;
  }

  if (*r == 0) {
    // Peer closed (EOF).
    handle_error(error::connection_reset);
    co_return;
  }

  parser_.commit(*r);

  for (;;) {
    auto parsed = parser_.parse_one();
    if (!parsed.has_value()) {
      if (parsed.error() == error::resp3_needs_more) {
        break;
      }

      // Deliver parser error into the pipeline, then treat it as a fatal connection error.
      if (pipeline_.has_pending_read()) {
        pipeline_.on_error(parsed.error());
      }
      handle_error(parsed.error());
      co_return;
    }

    if (!pipeline_.has_pending_read()) {
      // Unsolicited message (e.g. PUSH) is not supported yet.
      // Temporary policy: treat as "unsupported feature" rather than protocol violation.
      handle_error(error::unsolicited_message);
      co_return;
    }

    auto msg = resp3::build_message(parser_.tree(), *parsed);
    pipeline_.on_message(std::move(msg));

    // Critical for zero-copy parser: reclaim before parsing the next message.
    parser_.reclaim();
  }

  co_return;
}

inline auto connection::do_write() -> iocoro::awaitable<void> {
  if (state_ != connection_state::OPEN) {
    co_return;
  }

  struct in_flight_guard {
    bool& flag;
    explicit in_flight_guard(bool& f) : flag(f) {
      REDISCORO_ASSERT(!flag, "concurrent write detected");
      flag = true;
    }
    ~in_flight_guard() { flag = false; }
  };

  in_flight_guard guard{write_in_flight_};

  while (!cancel_.is_cancelled() && state_ == connection_state::OPEN && pipeline_.has_pending_write()) {
    auto view = pipeline_.next_write_buffer();
    auto buf = std::span<std::byte const>{
      reinterpret_cast<std::byte const*>(view.data()),
      view.size()
    };

    auto r = co_await socket_.async_write_some(buf);
    if (!r.has_value()) {
      // Socket IO error - treat as connection lost
      handle_error(error::connection_lost);
      co_return;
    }

    pipeline_.on_write_done(*r);
    if (pipeline_.has_pending_read()) {
      read_wakeup_.notify();
    }
  }

  co_return;
}

inline auto connection::handle_error(error ec) -> void {
  // Centralized runtime error path (step-5):
  // - Only OPEN may transition to FAILED (runtime IO errors after first OPEN).
  // - CONNECTING/INIT errors are handled by do_connect()/connect() and must not enter FAILED.
  // - Must NOT write CLOSED (only transition_to_closed()).

  if (state_ == connection_state::CLOSED || state_ == connection_state::CLOSING) {
    return;
  }

  if (state_ == connection_state::FAILED || state_ == connection_state::RECONNECTING) {
    return;
  }

  if (state_ != connection_state::OPEN) {
    // Non-OPEN state errors are not recorded in last_error_ (handled elsewhere).
    control_wakeup_.notify();
    return;
  }

  // OPEN runtime error -> FAILED.
  // Record error for diagnostics (user cannot obtain it directly).
  last_error_ = ec;
  state_ = connection_state::FAILED;
  auto clear_err = error::connection_lost;
  if (ec == error::request_timeout) {
    clear_err = error::request_timeout;
  }
  pipeline_.clear_all(clear_err);
  if (socket_.is_open()) {
    socket_.close();
  }
  control_wakeup_.notify();
  write_wakeup_.notify();
  read_wakeup_.notify();
}

inline auto connection::transition_to_closed() -> void {
  // Deterministic cleanup (idempotent).
  state_ = connection_state::CLOSED;

  pipeline_.clear_all(error::connection_closed);

  if (socket_.is_open()) {
    socket_.close();
  }
}

template <typename... Ts>
auto connection::enqueue(request req) -> std::shared_ptr<pending_response<Ts...>> {
  REDISCORO_ASSERT(req.reply_count() == sizeof...(Ts));
  auto slot = std::make_shared<pending_response<Ts...>>();

  // Thread-safety: enqueue() may be called from any executor/thread.
  // All state_ / pipeline_ mutation must happen on the connection strand.
  //
  // Performance: use dispatch() so if we're already on the strand, we run inline and avoid an
  // extra scheduling hop; otherwise it behaves like post().
  executor_.strand().any_executor().dispatch([self = shared_from_this(), req = std::move(req), slot]() mutable {
    self->enqueue_impl(std::move(req), slot.get());
  });

  return slot;
}

template <typename T>
auto connection::enqueue_dynamic(request req) -> std::shared_ptr<pending_dynamic_response<T>> {
  auto slot = std::make_shared<pending_dynamic_response<T>>(req.reply_count());

  // Thread-safety: enqueue_dynamic() may be called from any executor/thread.
  // All state_ / pipeline_ mutation must happen on the connection strand.
  executor_.strand().any_executor().dispatch([self = shared_from_this(), req = std::move(req), slot]() mutable {
    self->enqueue_impl(std::move(req), slot.get());
  });

  return slot;
}

}  // namespace rediscoro::detail
