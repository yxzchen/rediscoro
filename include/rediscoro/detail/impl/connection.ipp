#pragma once

#include <rediscoro/detail/connection.hpp>
#include <iocoro/bind_executor.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/this_coro.hpp>

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
    case connection_state::FAILED: {
      sink->fail_all(::rediscoro::error::connection_lost);
      return;
    }
    case connection_state::CLOSING:
    case connection_state::CLOSED: {
      sink->fail_all(::rediscoro::error::connection_closed);
      return;
    }
    case connection_state::OPEN:
    case connection_state::RECONNECTING: {
      break;
    }
  }

  pipeline_.push(std::move(req), sink);
  write_wakeup_.notify();
}

inline auto connection::actor_loop() -> iocoro::awaitable<void> {
  // Phase-1 minimal actor:
  // - No sub-loops yet.
  // - Exists so close() has a single join point (actor_awaitable_).
  //
  // Future hard constraint (IMPORTANT):
  // - When we add write_loop/read_loop/control_loop, actor_loop MUST own their lifetime.
  // - Do NOT spawn them as detached coroutines without joining, otherwise actor_loop could exit
  //   while sub-loops are still running/blocked.
  // - Preferred pattern: co_spawn(use_awaitable) each loop and co_await iocoro::when_all(...) to join.
  while (!cancel_.is_cancelled() && state_ != connection_state::CLOSED) {
    co_await control_wakeup_.wait();
  }

  // CRITICAL: Only transition_to_closed() is allowed to write state_ = CLOSED.
  transition_to_closed();
  co_return;
}

inline auto connection::write_loop() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - co_await write_wakeup_.wait() when no work / not OPEN
  // - while (state_ == OPEN && pipeline_.has_pending_write()) { drain write }
  // - on progress: notify read_wakeup_ if new pending reads become available
  // - on error: handle_error(ec) and notify control_wakeup_
  co_return;
}

inline auto connection::read_loop() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - if (state_ != OPEN || !pipeline_.has_pending_read()) { co_await read_wakeup_.wait(); continue; }
  // - async_read_some + parser + pipeline_.on_message/on_error
  // - on error: handle_error(ec) and notify control_wakeup_
  co_return;
}

inline auto connection::control_loop() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - Centralize state transitions: FAILED -> (sleep) -> RECONNECTING -> OPEN, cancel -> CLOSED
  // - On entering OPEN: notify read_wakeup_ and write_wakeup_
  // - On close(): ensure socket close + pipeline_.clear_all + notify loops to exit
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
  // TODO: Implementation
  // - async_read_some into buffer
  // - Feed data to parser
  // - Parse messages and dispatch to pipeline
  co_return;
}

inline auto connection::do_write() -> iocoro::awaitable<void> {
  // TODO: Implementation
  // - Get next_write_buffer from pipeline
  // - async_write to socket
  // - Notify pipeline of bytes written
  co_return;
}

inline auto connection::handle_error(std::error_code ec) -> void {
  // TODO: Implementation
  // - Log error
  // - Transition to FAILED state
  // - Clear pipeline with error
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
