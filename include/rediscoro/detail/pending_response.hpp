#pragma once

#include <rediscoro/assert.hpp>
#include <rediscoro/detail/notify_event.hpp>
#include <rediscoro/detail/response_builder.hpp>
#include <rediscoro/detail/response_sink.hpp>
#include <rediscoro/response.hpp>

#include <iocoro/awaitable.hpp>

#include <optional>
#include <utility>

namespace rediscoro::detail {

/// Pending response for a fixed-size pipeline (heterogeneous slots).
///
/// Implements response_sink to receive responses from pipeline.
///
/// Thread-safety model:
/// - deliver() and deliver_error() are called ONLY from connection strand
/// - wait() is called from user's coroutine context (any executor)
/// - No cross-executor synchronization needed for deliver
/// - notify_event handles executor dispatch for wait() resumption
///
/// Why this simplification is safe:
/// - pipeline runs on connection strand
/// - pipeline is the only caller of deliver()
/// - No concurrent deliver() calls possible
/// - wait() only reads result after notification
///
/// Responsibilities:
/// - Implement response_sink interface
/// - Aggregate N replies into response<Ts...>
/// - Provide awaitable interface via wait()
/// - Resume waiting coroutine on its original executor
///
/// Constraints:
/// - deliver() / deliver_error() can be called multiple times until expected replies are consumed
/// - deliver() MUST be called from connection strand
template <typename... Ts>
class pending_response final : public response_sink {
public:
  pending_response() = default;

  [[nodiscard]] auto expected_replies() const noexcept -> std::size_t override {
    return sizeof...(Ts);
  }

  [[nodiscard]] auto is_complete() const noexcept -> bool override {
    return result_.has_value();
  }

  auto wait() -> iocoro::awaitable<response<Ts...>>;

protected:
  auto do_deliver(resp3::message msg) -> void override;
  auto do_deliver_error(rediscoro::error err) -> void override;

private:
  notify_event event_{};
  response_builder<Ts...> builder_{};
  std::optional<response<Ts...>> result_{};
};

/// Pending response for a dynamic-size pipeline (homogeneous slots).
template <typename T>
class pending_dynamic_response final : public response_sink {
public:
  explicit pending_dynamic_response(std::size_t expected_count)
    : builder_(expected_count) {}

  [[nodiscard]] auto expected_replies() const noexcept -> std::size_t override {
    return builder_.expected_count();
  }

  [[nodiscard]] auto is_complete() const noexcept -> bool override {
    return result_.has_value();
  }

  auto wait() -> iocoro::awaitable<dynamic_response<T>>;

protected:
  auto do_deliver(resp3::message msg) -> void override;
  auto do_deliver_error(rediscoro::error err) -> void override;

private:
  notify_event event_{};
  dynamic_response_builder<T> builder_;
  std::optional<dynamic_response<T>> result_{};
};

}  // namespace rediscoro::detail

#include <rediscoro/detail/impl/pending_response.ipp>
