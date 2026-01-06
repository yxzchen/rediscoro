#pragma once

#include <rediscoro/detail/connection.hpp>
#include <rediscoro/resp3/builder.hpp>

#include <iocoro/bind_executor.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/this_coro.hpp>
#include <iocoro/when_all.hpp>

#include <cstddef>
#include <span>
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
    co_return make_error_code(::rediscoro::error::already_in_progress);
  }

  if (state_ == connection_state::CLOSED) {
    // Retry support: reset lifecycle state.
    state_ = connection_state::INIT;
    last_error_.reset();
    reconnect_count_ = 0;
    cancel_.reset();
  }

  if (cancel_.is_cancelled()) {
    co_return make_error_code(::rediscoro::error::operation_aborted);
  }

  if (!actor_awaitable_.has_value()) {
    run_actor();
  }

  state_ = connection_state::CONNECTING;
  last_error_.reset();

  // Not implemented yet: do_connect() currently does nothing.
  // We still call it to keep the control flow stable.
  co_await do_connect();

  if (cancel_.is_cancelled()) {
    // close() won; unify cleanup via close().
    co_await close();
    co_return make_error_code(::rediscoro::error::operation_aborted);
  }

  if (state_ == connection_state::OPEN) {
    co_return std::error_code{};
  }

  // Phase-1 deterministic failure.
  // Later milestones will set last_error_ precisely from resolver/connect/handshake.
  //
  // IMPORTANT semantic rule:
  // - Initial connect failure MUST NOT enter FAILED state (FAILED is reserved for runtime errors).
  // - connect() failure is reported by the returned error_code and ends in CLOSED via close().
  auto ec = make_error_code(::rediscoro::error::connect_failed);
  last_error_ = ec;
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
  pipeline_.clear_all(::rediscoro::error::connection_closed);

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
    co_await *actor_awaitable_;
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
      sink->fail_all(::rediscoro::error::not_connected);
      return;
    }
    case connection_state::FAILED:
    case connection_state::RECONNECTING: {
      sink->fail_all(::rediscoro::error::connection_lost);
      return;
    }
    case connection_state::CLOSING:
    case connection_state::CLOSED: {
      sink->fail_all(::rediscoro::error::connection_closed);
      return;
    }
    case connection_state::OPEN: {
      break;
    }
  }

  pipeline_.push(std::move(req), sink);
  write_wakeup_.notify();
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
    if (state_ != connection_state::OPEN || !pipeline_.has_pending_read()) {
      co_await read_wakeup_.wait();
      continue;
    }

    co_await do_read();
  }

  co_return;
}

inline auto connection::control_loop() -> iocoro::awaitable<void> {
  // Minimal control loop (step-3):
  // - Ensures control_wakeup_ is not a ghost and can be used as the centralized state driver.
  // - Reconnection will be implemented later; for now, runtime FAILED triggers deterministic shutdown.
  //
  // IMPORTANT constraints:
  // - Must NOT write CLOSED (only transition_to_closed()).
  // - Must be cancel-aware so close() can interrupt promptly.
  while (!cancel_.is_cancelled() && state_ != connection_state::CLOSED) {
    if (state_ == connection_state::FAILED) {
      // Phase-3 policy: deterministic shutdown on runtime error (no reconnection yet).
      state_ = connection_state::CLOSING;
      cancel_.request_cancel();
      write_wakeup_.notify();
      read_wakeup_.notify();
    }

    co_await control_wakeup_.wait();
  }

  co_return;
}

inline auto connection::do_connect() -> iocoro::awaitable<void> {
  // TODO: Implementation
  //
  // This method performs TCP connection + RESP3 handshake.
  // On error during initial connect(), it sets last_error_ with appropriate error code.
  // IMPORTANT: It MUST NOT set state_ = FAILED here; FAILED is reserved for runtime IO errors
  // after the connection has reached OPEN.
  //
  // 1. Resolve address
  //    - On DNS failure: set last_error_ = error::resolve_failed (or system error, by policy)
  //
  // 2. TCP connect with timeout
  //    - co_await async_connect(socket_, endpoint, timeout)
  //    - On timeout: set last_error_ = error::timeout, co_return
  //    - On error: set last_error_ = error::connect_failed (or system error, by policy), co_return
  //
  // 3. Check cancel (in case close() was called during connect)
  //    - if (cancel_.is_cancelled()) { last_error_ = error::operation_aborted; co_return; }
  //
  // 4. Send RESP3 handshake commands via pipeline
  //    CRITICAL: Use pipeline_.push() directly, NOT enqueue()
  //    (enqueue() rejects requests in CONNECTING state)
  //
  //    Commands to send:
  //    - HELLO 3 (switch to RESP3 protocol)
  //    - AUTH username password (if configured)
  //    - SELECT database (if database != 0)
  //    - CLIENT SETNAME name (if configured)
  //
  //    Implementation approach:
  //    a) Create pending_response for handshake
  //    b) Build request with all handshake commands
  //    c) pipeline_.push(request, pending_response*)
  //    d) co_await do_write() to send commands
  //    e) co_await do_read() to receive responses
  //    f) co_await pending_response->wait() to get results
  //    g) Validate each response
  //
  //    Error handling:
  //    - If response is -ERR: set last_error_ = error::handshake_failed, co_return
  //    - If timeout: set last_error_ = error::timeout, co_return
  //    - If socket error: set last_error_ = error::connect_failed (or system error, by policy), co_return
  //
  // 5. Handshake complete
  //    - state_ = OPEN
  //    - reconnect_count_ = 0 (reset on success)
  //    - co_return
  //
  // NOTE: Handshake uses pipeline for request/response pairing and unified error handling.
  // User requests are still rejected by enqueue() during CONNECTING state.
  co_return;
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

  while (!cancel_.is_cancelled() && state_ == connection_state::OPEN && pipeline_.has_pending_read()) {
    auto writable = parser_.prepare();
    auto buf = std::span<std::byte>{
      reinterpret_cast<std::byte*>(writable.data()),
      writable.size()
    };

    auto r = co_await socket_.async_read_some(buf);
    if (!r.has_value()) {
      handle_error(r.error());
      co_return;
    }

    if (*r == 0) {
      // Peer closed.
      handle_error(std::make_error_code(std::errc::connection_reset));
      co_return;
    }

    parser_.commit(*r);

    for (;;) {
      auto parsed = parser_.parse_one();
      if (!parsed.has_value()) {
        if (parsed.error() == resp3::error::needs_more) {
          break;
        }

        // Deliver parser error into the pipeline, then treat it as a fatal connection error.
        if (pipeline_.has_pending_read()) {
          pipeline_.on_error(parsed.error());
        }
        handle_error(resp3::make_error_code(parsed.error()));
        co_return;
      }

      if (!pipeline_.has_pending_read()) {
        // Unsolicited message (e.g. PUSH) is not supported yet.
        // Temporary policy: treat as "unsupported feature" rather than protocol violation.
        handle_error(std::make_error_code(std::errc::not_supported));
        co_return;
      }

      auto msg = resp3::build_message(parser_.tree(), *parsed);
      pipeline_.on_message(std::move(msg));

      // Critical for zero-copy parser: reclaim before parsing the next message.
      parser_.reclaim();

      if (!pipeline_.has_pending_read()) {
        break;
      }
    }
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
      handle_error(r.error());
      co_return;
    }

    pipeline_.on_write_done(*r);
    if (pipeline_.has_pending_read()) {
      read_wakeup_.notify();
    }
  }

  co_return;
}

inline auto connection::handle_error(std::error_code ec) -> void {
  // Minimal error handling for step-2:
  // - Only OPEN runtime IO errors should transition to FAILED.
  // - FAILED/RECONNECTING/CLOSING/CLOSED are terminal-ish for normal IO.

  if (state_ == connection_state::FAILED ||
      state_ == connection_state::RECONNECTING ||
      state_ == connection_state::CLOSING ||
      state_ == connection_state::CLOSED) {
    return;
  }

  last_error_ = ec;

  if (state_ == connection_state::OPEN) {
    state_ = connection_state::FAILED;
    pipeline_.clear_all(::rediscoro::error::connection_lost);
    if (socket_.is_open()) {
      socket_.close();
    }
    control_wakeup_.notify();
    write_wakeup_.notify();
    read_wakeup_.notify();
    return;
  }

  // During CONNECTING/INIT, do_connect() is responsible for mapping errors; keep state as-is.
  cancel_.request_cancel();
  control_wakeup_.notify();
}

inline auto connection::transition_to_closed() -> void {
  // Deterministic cleanup (idempotent).
  state_ = connection_state::CLOSED;

  pipeline_.clear_all(::rediscoro::error::connection_closed);

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
  executor_.strand().any_executor().post(
    [self = shared_from_this(), req = std::move(req), slot]() mutable {
      self->enqueue_impl(std::move(req), slot.get());
    }
  );

  return slot;
}

template <typename T>
auto connection::enqueue_dynamic(request req) -> std::shared_ptr<pending_dynamic_response<T>> {
  auto slot = std::make_shared<pending_dynamic_response<T>>(req.reply_count());

  // Thread-safety: enqueue_dynamic() may be called from any executor/thread.
  // All state_ / pipeline_ mutation must happen on the connection strand.
  executor_.strand().any_executor().post(
    [self = shared_from_this(), req = std::move(req), slot]() mutable {
      self->enqueue_impl(std::move(req), slot.get());
    }
  );

  return slot;
}

}  // namespace rediscoro::detail
