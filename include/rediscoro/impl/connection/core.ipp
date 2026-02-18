#pragma once

#include <rediscoro/detail/connection.hpp>
#include <rediscoro/error_info.hpp>

#include <iocoro/bind_executor.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/this_coro.hpp>

#include <cmath>
#include <exception>
#include <string>
#include <utility>

namespace rediscoro::detail {

inline auto sanitize_reconnection_policy(reconnection_policy policy) -> reconnection_policy {
  if (policy.immediate_attempts < 0) {
    policy.immediate_attempts = 0;
  }

  if (policy.initial_delay.count() < 0) {
    policy.initial_delay = std::chrono::milliseconds{0};
  }

  if (policy.max_delay.count() < 0) {
    policy.max_delay = std::chrono::milliseconds{0};
  }

  if (policy.max_delay < policy.initial_delay) {
    policy.max_delay = policy.initial_delay;
  }

  if (!std::isfinite(policy.backoff_factor) || policy.backoff_factor <= 1.0) {
    policy.backoff_factor = 2.0;
  }

  if (!std::isfinite(policy.jitter_ratio) || policy.jitter_ratio < 0.0) {
    policy.jitter_ratio = 0.0;
  } else if (policy.jitter_ratio > 1.0) {
    policy.jitter_ratio = 1.0;
  }

  return policy;
}

inline auto make_internal_error(std::exception_ptr ep, std::string_view context) -> error_info {
  std::string detail;
  if (!context.empty()) {
    detail.append(context.data(), context.size());
    detail.append(": ");
  }

  if (ep == nullptr) {
    detail.append("unknown exception");
    return {client_errc::internal_error, std::move(detail)};
  }

  try {
    std::rethrow_exception(ep);
  } catch (std::exception const& e) {
    detail.append(e.what());
  } catch (...) {
    detail.append("unknown exception");
  }

  return {client_errc::internal_error, std::move(detail)};
}

inline auto make_internal_error_from_current_exception(std::string_view context) -> error_info {
  return make_internal_error(std::current_exception(), context);
}

inline auto fail_sink_with_current_exception(std::shared_ptr<response_sink> const& sink,
                                             std::string_view context) noexcept -> void {
  if (!sink) {
    return;
  }
  auto err = make_internal_error_from_current_exception(context);
  try {
    sink->fail_all(std::move(err));
  } catch (...) {
    REDISCORO_LOG_WARNING("sink fail_all threw while handling exception context={}", context);
  }
}

inline connection::connection(iocoro::any_io_executor ex, config cfg)
    : cfg_(std::move(cfg)),
      executor_(ex),
      socket_(executor_.get_io_executor()),
      pipeline_(pipeline::limits{
        .max_requests = cfg_.max_pipeline_requests,
        .max_pending_write_bytes = cfg_.max_pipeline_pending_write_bytes,
      }),
      parser_(resp3::parser::limits{
        .max_resp_bulk_bytes = cfg_.max_resp_bulk_bytes,
        .max_resp_container_len = cfg_.max_resp_container_len,
        .max_resp_line_bytes = cfg_.max_resp_line_bytes,
      }) {
  cfg_.reconnection = sanitize_reconnection_policy(cfg_.reconnection);
}

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
    set_state(connection_state::CLOSING);
  }

  pipeline_.clear_all(client_errc::connection_closed);

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
  REDISCORO_LOG_INFO("actor_start state={}", static_cast<int>(state_));

  auto ex = executor_.strand().executor();
  auto self = shared_from_this();
  iocoro::co_spawn(
    ex, stop_.get_token(),
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
          auto err = make_internal_error(r.error(), "connection actor");
          auto const from = self->state_;
          REDISCORO_LOG_WARNING(
            "actor_exception state={} err_code={} reconnect_count={} generation={}",
            static_cast<int>(from), err.code.value(), self->reconnect_count_, self->generation_);
          if (self->state_ != connection_state::CLOSING &&
              self->state_ != connection_state::CLOSED) {
            self->emit_connection_event(connection_event{
              .kind = connection_event_kind::disconnected,
              .stage = connection_event_stage::actor,
              .from_state = static_cast<std::int32_t>(from),
              .to_state = static_cast<std::int32_t>(connection_state::CLOSING),
              .error = err,
            });
          }
          self->pipeline_.clear_all(err);
          if (self->socket_.is_open()) {
            (void)self->socket_.close();
          }
          if (self->state_ != connection_state::CLOSED) {
            self->set_state(connection_state::CLOSING);
          }
          self->write_wakeup_.notify();
          self->read_wakeup_.notify();
          self->control_wakeup_.notify();
        }

        if (self->state_ != connection_state::CLOSED) {
          self->transition_to_closed();
        }
        self->actor_done_.notify();
      });
    });
}

inline auto connection::connect() -> iocoro::awaitable<expected<void, error_info>> {
  co_await iocoro::this_coro::switch_to(executor_.strand().executor());

  if (state_ == connection_state::OPEN) {
    co_return expected<void, error_info>{};
  }

  if (state_ == connection_state::CONNECTING) {
    co_return unexpected(client_errc::already_in_progress);
  }

  if (state_ == connection_state::CLOSED) {
    // Retry support: reset lifecycle state.
    REDISCORO_LOG_INFO("state_transition from={} to={}", static_cast<int>(connection_state::CLOSED),
                       static_cast<int>(connection_state::INIT));
    set_state(connection_state::INIT);
    reconnect_count_ = 0;
    stop_.reset();
  }

  if (stop_.get_token().stop_requested()) {
    co_return unexpected(client_errc::operation_aborted);
  }

  if (!actor_running_) {
    run_actor();
  }

  REDISCORO_LOG_INFO("state_transition from={} to={}", static_cast<int>(state_),
                     static_cast<int>(connection_state::CONNECTING));
  set_state(connection_state::CONNECTING);

  // Attempt connection. do_connect() returns unexpected(error) on failure.
  auto connect_res = co_await iocoro::co_spawn(executor_.strand().executor(), stop_.get_token(),
                                               do_connect(), iocoro::use_awaitable);
  if (!connect_res) {
    REDISCORO_LOG_WARNING("initial_connect_failed err_code={}", connect_res.error().code.value());
    emit_connection_event(connection_event{
      .kind = connection_event_kind::disconnected,
      .stage = connection_event_stage::connect,
      .from_state = static_cast<std::int32_t>(connection_state::CONNECTING),
      .to_state = static_cast<std::int32_t>(connection_state::CLOSING),
      .error = connect_res.error(),
    });
    // Initial connect failure MUST NOT enter FAILED state (FAILED is reserved for runtime errors).
    // Cleanup is unified via close() (joins the actor).
    co_await close();
    co_return unexpected(std::move(connect_res.error()));
  }

  // Successful do_connect() implies OPEN.
  REDISCORO_ASSERT(state_ == connection_state::OPEN);

  // Wake IO loops that might be waiting for the OPEN transition.
  read_wakeup_.notify();
  write_wakeup_.notify();
  control_wakeup_.notify();
  co_return expected<void, error_info>{};
}

inline auto connection::close() -> iocoro::awaitable<void> {
  co_await iocoro::this_coro::switch_to(executor_.strand().executor());

  if (state_ == connection_state::CLOSED) {
    co_return;
  }

  // Phase-1: determinism-first shutdown.
  stop_.request_stop();
  REDISCORO_LOG_INFO("state_transition from={} to={}", static_cast<int>(state_),
                     static_cast<int>(connection_state::CLOSING));
  set_state(connection_state::CLOSING);

  // Fail all pending work deterministically.
  pipeline_.clear_all(client_errc::connection_closed);

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

inline auto connection::enqueue_impl(request req, std::shared_ptr<response_sink> sink,
                                     std::chrono::steady_clock::time_point start) -> void {
  REDISCORO_ASSERT(sink != nullptr);

  auto const hooks = cfg_.trace_hooks;  // copy: stable for the sink and callbacks
  const bool tracing = hooks.enabled();

  request_trace_info trace_info{};
  if (tracing) {
    trace_info = request_trace_info{
      .id = next_request_id_++,
      .kind = request_kind::user,
      .command_count = req.command_count(),
      .wire_bytes = req.wire().size(),
    };

    if (hooks.on_start != nullptr) {
      request_trace_start evt{.info = trace_info};
      try {
        hooks.on_start(hooks.user_data, evt);
      } catch (...) {
        REDISCORO_LOG_WARNING("trace on_start callback threw: request_id={}, kind={}",
                              trace_info.id, static_cast<unsigned>(trace_info.kind));
      }
    }
  }

  auto reject = [&](error_info err) -> void {
    if (tracing && hooks.on_finish != nullptr) {
      request_trace_finish evt{
        .info = trace_info,
        .duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - start),
        .ok_count = 0,
        .error_count = sink->expected_replies(),
        .primary_error = err.code,
        .primary_error_detail = err.detail,
      };
      try {
        hooks.on_finish(hooks.user_data, evt);
      } catch (...) {
        REDISCORO_LOG_WARNING("trace on_finish callback threw: request_id={}, kind={}",
                              trace_info.id, static_cast<unsigned>(trace_info.kind));
      }
    }
    sink->fail_all(std::move(err));
  };

  // State gating: reject early if not ready.
  switch (state_) {
    case connection_state::INIT:
    case connection_state::CONNECTING: {
      reject(client_errc::not_connected);
      return;
    }
    case connection_state::FAILED:
    case connection_state::RECONNECTING: {
      reject(client_errc::connection_lost);
      return;
    }
    case connection_state::CLOSING:
    case connection_state::CLOSED: {
      reject(client_errc::connection_closed);
      return;
    }
    case connection_state::OPEN: {
      break;
    }
  }

  pipeline::time_point deadline = pipeline::time_point::max();
  if (cfg_.request_timeout.has_value()) {
    deadline = pipeline::clock::now() + *cfg_.request_timeout;
  }
  if (!pipeline_.push(std::move(req), sink, deadline)) {
    reject(client_errc::queue_full);
    return;
  }
  if (tracing) {
    sink->set_trace_context(hooks, trace_info, start);
  }
  write_wakeup_.notify();
  // request_timeout scheduling / wake control_loop when first request arrives
  control_wakeup_.notify();
}

inline auto connection::emit_connection_event(connection_event evt) noexcept -> void {
  auto const hooks = cfg_.connection_hooks;
  if (!hooks.enabled()) {
    return;
  }

  if (evt.timestamp == std::chrono::steady_clock::time_point{}) {
    evt.timestamp = std::chrono::steady_clock::now();
  }
  evt.generation = generation_;
  evt.reconnect_count = reconnect_count_;

  try {
    hooks.on_event(hooks.user_data, evt);
  } catch (...) {
    REDISCORO_LOG_WARNING("connection on_event callback threw: kind={}, stage={}",
                          static_cast<unsigned>(evt.kind), static_cast<unsigned>(evt.stage));
  }
}

inline auto connection::transition_to_closed() -> void {
  // Deterministic cleanup (idempotent).
  auto const from = state_;
  REDISCORO_LOG_INFO("state_transition from={} to={}", static_cast<int>(from),
                     static_cast<int>(connection_state::CLOSED));
  set_state(connection_state::CLOSED);

  pipeline_.clear_all(client_errc::connection_closed);

  if (socket_.is_open()) {
    (void)socket_.close();
  }

  emit_connection_event(connection_event{
    .kind = connection_event_kind::closed,
    .stage = connection_event_stage::close,
    .from_state = static_cast<std::int32_t>(from),
    .to_state = static_cast<std::int32_t>(connection_state::CLOSED),
  });
}

}  // namespace rediscoro::detail
