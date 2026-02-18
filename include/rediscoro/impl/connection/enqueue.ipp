#pragma once

#include <rediscoro/detail/connection.hpp>

#include <memory>
#include <utility>

namespace rediscoro::detail {

template <typename... Ts>
inline auto connection::enqueue(request req) -> std::shared_ptr<pending_response<Ts...>> {
  REDISCORO_ASSERT(req.reply_count() == sizeof...(Ts));
  auto slot = std::make_shared<pending_response<Ts...>>();
  REDISCORO_LOG_DEBUG(
    "enqueue api fixed request: command_count={} wire_bytes={} expected_replies={}",
    req.command_count(), req.wire().size(), sizeof...(Ts));

  const bool need_trace = cfg_.trace_hooks.enabled();
  const auto start =
    need_trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

  // Thread-safety: enqueue() may be called from any executor/thread.
  // All state_ / pipeline_ mutation must happen on the connection strand.
  //
  // Performance: use dispatch() so if we're already on the strand, we run inline and avoid an
  // extra scheduling hop; otherwise it behaves like post().
  executor_.strand().executor().dispatch(
    [self = shared_from_this(), req = std::move(req), slot, start]() mutable {
      try {
        self->enqueue_impl(std::move(req), slot, start);
      } catch (...) {
        REDISCORO_LOG_ERROR("enqueue api fixed dispatch exception");
        fail_sink_with_current_exception(slot, "enqueue dispatch");
      }
    });

  return slot;
}

template <typename T>
inline auto connection::enqueue_dynamic(request req)
  -> std::shared_ptr<pending_dynamic_response<T>> {
  auto slot = std::make_shared<pending_dynamic_response<T>>(req.reply_count());
  REDISCORO_LOG_DEBUG(
    "enqueue api dynamic request: command_count={} wire_bytes={} expected_replies={}",
    req.command_count(), req.wire().size(), req.reply_count());

  const bool need_trace = cfg_.trace_hooks.enabled();
  const auto start =
    need_trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

  // Thread-safety: enqueue_dynamic() may be called from any executor/thread.
  // All state_ / pipeline_ mutation must happen on the connection strand.
  executor_.strand().executor().dispatch(
    [self = shared_from_this(), req = std::move(req), slot, start]() mutable {
      try {
        self->enqueue_impl(std::move(req), slot, start);
      } catch (...) {
        REDISCORO_LOG_ERROR("enqueue api dynamic dispatch exception");
        fail_sink_with_current_exception(slot, "enqueue_dynamic dispatch");
      }
    });

  return slot;
}

}  // namespace rediscoro::detail
