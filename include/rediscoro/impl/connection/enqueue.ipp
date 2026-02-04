#pragma once

#include <rediscoro/detail/connection.hpp>

#include <memory>
#include <utility>

namespace rediscoro::detail {

template <typename... Ts>
inline auto connection::enqueue(request req) -> std::shared_ptr<pending_response<Ts...>> {
  REDISCORO_ASSERT(req.reply_count() == sizeof...(Ts));
  auto slot = std::make_shared<pending_response<Ts...>>();

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
      self->enqueue_impl(std::move(req), std::move(slot), start);
    });

  return slot;
}

template <typename T>
inline auto connection::enqueue_dynamic(request req)
  -> std::shared_ptr<pending_dynamic_response<T>> {
  auto slot = std::make_shared<pending_dynamic_response<T>>(req.reply_count());

  const bool need_trace = cfg_.trace_hooks.enabled();
  const auto start =
    need_trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

  // Thread-safety: enqueue_dynamic() may be called from any executor/thread.
  // All state_ / pipeline_ mutation must happen on the connection strand.
  executor_.strand().executor().dispatch(
    [self = shared_from_this(), req = std::move(req), slot, start]() mutable {
      self->enqueue_impl(std::move(req), std::move(slot), start);
    });

  return slot;
}

}  // namespace rediscoro::detail
