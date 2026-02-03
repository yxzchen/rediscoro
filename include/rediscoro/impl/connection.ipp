#pragma once

#include <rediscoro/detail/connection.hpp>
#include <rediscoro/ignore.hpp>
#include <rediscoro/resp3/builder.hpp>

#include <iocoro/bind_executor.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/ip/resolver.hpp>
#include <iocoro/steady_timer.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/when_all.hpp>
#include <iocoro/when_any.hpp>
#include <iocoro/with_timeout.hpp>

#include <cmath>
#include <cstddef>
#include <exception>
#include <span>
#include <string>
#include <system_error>

namespace rediscoro::detail {

inline connection::connection(iocoro::any_io_executor ex, config cfg)
    : cfg_(std::move(cfg)), executor_(ex), socket_(executor_.get_io_executor()) {}

inline connection::~connection() noexcept {
  // Best-effort synchronous cleanup.
  //
  // Lifetime model (CRITICAL):
  // - actor_loop() captures shared_from_this() to keep *this alive until the actor completes.
  // - Therefore, ~connection() should only run when the actor is no longer running.
  //
  // This destructor does not co_await (cannot); it only closes resources and notifies waiters.
  //
  stop_.request_stop();

  // Do not force CLOSED here (only transition_to_closed() may write CLOSED).
  // We only ensure resources are released.
  if (state_ != connection_state::CLOSED) {
    state_ = connection_state::CLOSING;
  }

  pipeline_.clear_all(error::connection_closed);

  if (socket_.is_open()) {
    (void)socket_.close();
  }

  write_wakeup_.notify();
  read_wakeup_.notify();
  control_wakeup_.notify();
}

inline auto connection::run_actor() -> void {
  REDISCORO_ASSERT(!actor_running_ && "run_actor() called while actor is running");
  actor_running_ = true;

  auto ex = executor_.strand().executor();
  auto self = shared_from_this();
  iocoro::co_spawn(
    ex, stop_.token(),
    [self, ex]() mutable -> iocoro::awaitable<void> {
      // Keep the connection alive until the actor completes.
      (void)self;
      co_return co_await iocoro::bind_executor(ex, self->actor_loop());
    },
    [self, ex](iocoro::expected<void, std::exception_ptr> r) mutable {
      // Ensure lifecycle mutations are serialized on the connection strand.
      ex.post([self = std::move(self), r = std::move(r)]() mutable {
        self->actor_running_ = false;
        if (!r) {
          // No-exception policy: record a best-effort diagnostic and proceed with deterministic cleanup.
          self->last_error_ = error::connection_lost;
        }
        if (self->state_ != connection_state::CLOSED) {
          self->transition_to_closed();
        }
        self->actor_done_.notify();
      });
    });
}

inline auto connection::connect() -> iocoro::awaitable<expected<void, error>> {
  co_await iocoro::this_coro::switch_to(executor_.strand().executor());

  if (state_ == connection_state::OPEN) {
    co_return expected<void, error>{};
  }

  if (state_ == connection_state::CONNECTING) {
    co_return unexpected(error::already_in_progress);
  }

  if (state_ == connection_state::CLOSED) {
    // Retry support: reset lifecycle state.
    state_ = connection_state::INIT;
    last_error_.reset();
    reconnect_count_ = 0;
    stop_.reset();
  }

  if (stop_.token().stop_requested()) {
    co_return unexpected(error::operation_aborted);
  }

  if (!actor_running_) {
    run_actor();
  }

  state_ = connection_state::CONNECTING;

  // Attempt connection. do_connect() returns unexpected(error) on failure.
  auto connect_res = co_await iocoro::co_spawn(executor_.strand().executor(), stop_.token(),
                                               do_connect(), iocoro::use_awaitable);
  if (!connect_res) {
    // Initial connect failure MUST NOT enter FAILED state (FAILED is reserved for runtime errors).
    // Cleanup is unified via close() (joins the actor).
    co_await close();
    co_return unexpected(connect_res.error());
  }

  // Successful do_connect() implies OPEN.
  REDISCORO_ASSERT(state_ == connection_state::OPEN);

  // Wake IO loops that might be waiting for the OPEN transition.
  read_wakeup_.notify();
  write_wakeup_.notify();
  control_wakeup_.notify();
  co_return expected<void, error>{};
}

inline auto connection::close() -> iocoro::awaitable<void> {
  co_await iocoro::this_coro::switch_to(executor_.strand().executor());

  if (state_ == connection_state::CLOSED) {
    co_return;
  }

  // Phase-1: determinism-first shutdown.
  stop_.request_stop();
  state_ = connection_state::CLOSING;

  // Fail all pending work deterministically.
  pipeline_.clear_all(error::connection_closed);

  // Close socket immediately.
  if (socket_.is_open()) {
    (void)socket_.close();
  }

  // Wake loops / actor.
  write_wakeup_.notify();
  read_wakeup_.notify();
  control_wakeup_.notify();

  if (actor_running_) {
    (void)co_await actor_done_.async_wait();
  }

  REDISCORO_ASSERT(state_ == connection_state::CLOSED);
  co_return;
}

inline auto connection::enqueue_impl(request req, std::shared_ptr<response_sink> sink) -> void {
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
  pipeline_.push(std::move(req), std::move(sink), deadline);
  write_wakeup_.notify();
  // request_timeout scheduling / wake control_loop when first request arrives
  control_wakeup_.notify();
}

inline auto connection::transition_to_closed() -> void {
  // Deterministic cleanup (idempotent).
  state_ = connection_state::CLOSED;

  pipeline_.clear_all(error::connection_closed);

  if (socket_.is_open()) {
    (void)socket_.close();
  }
}

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
        handle_error(error::request_timeout);
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
    state_ = connection_state::RECONNECTING;
    auto reconnect_res = co_await do_connect();
    if (!reconnect_res) {
      // Failed attempt: transition back to FAILED and schedule next delay.
      // Store the reconnection error for diagnostics (user cannot obtain it directly).
      state_ = connection_state::FAILED;
      last_error_ = reconnect_res.error();
      reconnect_count_ += 1;
      continue;
    }

    // Successful do_connect() implies OPEN.
    REDISCORO_ASSERT(state_ == connection_state::OPEN);

    reconnect_count_ = 0;
    last_error_.reset();  // Clear error on successful reconnect
    read_wakeup_.notify();
    write_wakeup_.notify();
    control_wakeup_.notify();
    co_return;
  }
}

inline auto connection::do_connect() -> iocoro::awaitable<expected<void, error>> {
  auto tok = co_await iocoro::this_coro::stop_token;
  if (tok.stop_requested()) {
    co_return unexpected(error::operation_aborted);
  }

  // Defensive: ensure parser state is clean at the start of a handshake.
  // This prevents accidental carry-over between retries or reconnect attempts.
  parser_.reset();

  // Resolve:
  // - iocoro resolver runs getaddrinfo on a background thread_pool and resumes on this coroutine's
  //   executor (our strand).
  // - Cancellation is best-effort via stop_token. It cannot interrupt an in-flight getaddrinfo()
  //   but can prevent delivering results to the awaiting coroutine.
  iocoro::ip::tcp::resolver resolver{};
  auto res = co_await iocoro::with_timeout(
    resolver.async_resolve(cfg_.host, std::to_string(cfg_.port)), cfg_.resolve_timeout);
  if (!res) {
    if (res.error() == iocoro::error::timed_out) {
      co_return unexpected(error::resolve_timeout);
    } else if (res.error() == iocoro::error::operation_aborted) {
      co_return unexpected(error::operation_aborted);
    } else {
      co_return unexpected(error::resolve_failed);
    }
  }
  if (res->empty()) {
    co_return unexpected(error::resolve_failed);
  }

  if (tok.stop_requested()) {
    co_return unexpected(error::operation_aborted);
  }

  // TCP connect with timeout (iterate endpoints in order).
  std::error_code connect_ec{};
  for (auto const& ep : *res) {
    // IMPORTANT: after a failed connect attempt, the socket may be left in a platform-dependent
    // error state. Always close before trying the next endpoint.
    if (socket_.is_open()) {
      (void)socket_.close();
    }

    auto connect_res =
      co_await iocoro::with_timeout(socket_.async_connect(ep), cfg_.connect_timeout);
    if (connect_res) {
      connect_ec = {};
      break;
    }
    connect_ec = connect_res.error();
  }

  if (connect_ec) {
    // Map timeout/cancel vs generic connect failure.
    if (connect_ec == iocoro::error::timed_out) {
      co_return unexpected(error::connect_timeout);
    } else if (connect_ec == iocoro::error::operation_aborted) {
      co_return unexpected(error::operation_aborted);
    } else {
      co_return unexpected(error::connect_failed);
    }
  }

  if (tok.stop_requested()) {
    co_return unexpected(error::operation_aborted);
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
  pipeline_.push(std::move(req), slot);

  // Drive handshake IO directly (read/write loops are gated on OPEN so they will not interfere).
  auto handshake_res = co_await iocoro::with_timeout(
    [&]() -> iocoro::awaitable<iocoro::result<void>> {
      // Phase-1: flush the full handshake request first.
      // Handshake generates no additional writes after the initial request is fully sent.
      while (!tok.stop_requested() && pipeline_.has_pending_write()) {
        auto view = pipeline_.next_write_buffer();
        auto buf = std::as_bytes(std::span{view.data(), view.size()});
        auto w = co_await socket_.async_write_some(buf);
        if (!w) {
          if (w.error() == iocoro::error::operation_aborted) {
            co_return iocoro::unexpected(error::operation_aborted);
          }
          co_return iocoro::unexpected(error::handshake_failed);
        }
        pipeline_.on_write_done(*w);
      }

      // Phase-2: read/parse until the handshake sink completes.
      while (!tok.stop_requested() && !slot->is_complete()) {
        auto writable = parser_.prepare();
        auto r = co_await socket_.async_read_some(writable);
        if (!r) {
          if (r.error() == iocoro::error::operation_aborted) {
            co_return iocoro::unexpected(error::operation_aborted);
          }
          co_return iocoro::unexpected(error::handshake_failed);
        }
        if (*r == 0) {
          co_return iocoro::unexpected(error::connection_reset);
        }
        parser_.commit(*r);

        for (;;) {
          auto parsed = parser_.parse_one();
          if (!parsed) {
            if (parsed.error() == error::resp3_needs_more) {
              break;
            }
            if (pipeline_.has_pending_read()) {
              pipeline_.on_error(parsed.error());
            }
            co_return iocoro::unexpected(parsed.error());
          }

          if (!pipeline_.has_pending_read()) {
            co_return iocoro::unexpected(error::unsolicited_message);
          }

          auto msg = resp3::build_message(parser_.tree(), *parsed);
          pipeline_.on_message(std::move(msg));
          parser_.reclaim();

          if (slot->is_complete()) {
            break;
          }
        }
      }

      if (tok.stop_requested()) {
        co_return iocoro::unexpected(error::operation_aborted);
      }

      co_return iocoro::ok();
    }(),
    cfg_.connect_timeout);

  if (!handshake_res) {
    auto const ec = handshake_res.error();
    if (ec == iocoro::error::timed_out) {
      pipeline_.clear_all(error::handshake_timeout);
      co_return unexpected(error::handshake_timeout);
    }
    if (ec == iocoro::error::operation_aborted) {
      pipeline_.clear_all(error::operation_aborted);
      co_return unexpected(error::operation_aborted);
    }
    if (ec.category() == rediscoro::detail::category()) {
      auto const e = static_cast<error>(ec.value());
      if (e == error::unsolicited_message) {
        pipeline_.clear_all(error::handshake_failed);
        co_return unexpected(error::handshake_failed);
      }
      if (e == error::operation_aborted) {
        pipeline_.clear_all(error::operation_aborted);
        co_return unexpected(error::operation_aborted);
      }
      pipeline_.clear_all(error::handshake_failed);
      co_return unexpected(e);
    }

    pipeline_.clear_all(error::handshake_failed);
    co_return unexpected(error::handshake_failed);
  }

  // Validate all handshake replies: any error => handshake_failed.
  //
  // Defensive: handshake_res == ok() implies slot should be complete (loop condition). Keep this
  // check to avoid future hangs if the handshake loop logic changes.
  if (!slot->is_complete()) {
    pipeline_.clear_all(error::handshake_failed);
    co_return unexpected(error::handshake_failed);
  }
  auto results = co_await slot->wait();
  for (std::size_t i = 0; i < results.size(); ++i) {
    if (!results[i]) {
      pipeline_.clear_all(error::handshake_failed);
      co_return unexpected(error::handshake_failed);
    }
  }

  // Handshake succeeded.
  state_ = connection_state::OPEN;
  reconnect_count_ = 0;

  // Defensive: ensure parser buffer/state is clean when handing over to runtime loops.
  parser_.reset();
  co_return expected<void, error>{};
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
  auto r = co_await socket_.async_read_some(writable);
  if (!r) {
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
    if (!parsed) {
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

  auto tok = co_await iocoro::this_coro::stop_token;
  while (!tok.stop_requested() && state_ == connection_state::OPEN &&
         pipeline_.has_pending_write()) {
    auto view = pipeline_.next_write_buffer();
    auto buf = std::as_bytes(std::span{view.data(), view.size()});

    auto r = co_await socket_.async_write_some(buf);
    if (!r) {
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
  // Centralized runtime error path:
  // - Only OPEN may transition to FAILED (runtime IO errors after first OPEN).
  // - CONNECTING/INIT errors are handled by do_connect()/connect() and must not enter FAILED.
  // - Must NOT write CLOSED (only transition_to_closed()).

  if (state_ == connection_state::CLOSED || state_ == connection_state::CLOSING) {
    return;
  }

  if (state_ == connection_state::FAILED || state_ == connection_state::RECONNECTING) {
    return;
  }

  REDISCORO_ASSERT(state_ == connection_state::OPEN,
                   "handle_error must not be used for CONNECTING/INIT errors");
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
    (void)socket_.close();
  }
  control_wakeup_.notify();
  write_wakeup_.notify();
  read_wakeup_.notify();
}

template <typename... Ts>
inline auto connection::enqueue(request req) -> std::shared_ptr<pending_response<Ts...>> {
  REDISCORO_ASSERT(req.reply_count() == sizeof...(Ts));
  auto slot = std::make_shared<pending_response<Ts...>>();

  // Thread-safety: enqueue() may be called from any executor/thread.
  // All state_ / pipeline_ mutation must happen on the connection strand.
  //
  // Performance: use dispatch() so if we're already on the strand, we run inline and avoid an
  // extra scheduling hop; otherwise it behaves like post().
  executor_.strand().executor().dispatch(
    [self = shared_from_this(), req = std::move(req), slot]() mutable {
      self->enqueue_impl(std::move(req), std::move(slot));
    });

  return slot;
}

template <typename T>
inline auto connection::enqueue_dynamic(request req)
  -> std::shared_ptr<pending_dynamic_response<T>> {
  auto slot = std::make_shared<pending_dynamic_response<T>>(req.reply_count());

  // Thread-safety: enqueue_dynamic() may be called from any executor/thread.
  // All state_ / pipeline_ mutation must happen on the connection strand.
  executor_.strand().executor().dispatch(
    [self = shared_from_this(), req = std::move(req), slot]() mutable {
      self->enqueue_impl(std::move(req), std::move(slot));
    });

  return slot;
}

}  // namespace rediscoro::detail
