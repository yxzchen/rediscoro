#pragma once

#include <rediscoro/detail/connection_state.hpp>
#include <rediscoro/error_info.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <system_error>

namespace rediscoro {

/// Classify the origin of a request for tracing.
enum class request_kind : std::uint8_t {
  user = 0,
  handshake = 1,
};

/// Minimal request metadata for tracing callbacks.
struct request_trace_info {
  std::uint64_t id{};
  request_kind kind{request_kind::user};
  std::size_t command_count{0};
  std::size_t wire_bytes{0};
};

struct request_trace_start {
  request_trace_info info{};
};

struct request_trace_finish {
  request_trace_info info{};
  // End-to-end time including time spent queued before dispatch on the connection strand.
  std::chrono::nanoseconds duration{};

  std::size_t ok_count{0};
  std::size_t error_count{0};

  /// The first observed error (if any). Default constructed on success.
  std::error_code primary_error{};

  /// Human-oriented detail (if any). Lifetime: valid only during the callback.
  std::string_view primary_error_detail{};
};

/// Lightweight tracing hooks (no logging dependency).
///
/// Threading / performance contract:
/// - Callbacks are invoked on the connection strand.
/// - Implementations MUST be non-blocking and MUST NOT throw.
struct request_trace_hooks {
  using on_start_fn = void (*)(void*, request_trace_start const&);
  using on_finish_fn = void (*)(void*, request_trace_finish const&);

  void* user_data{};
  on_start_fn on_start{};
  on_finish_fn on_finish{};

  [[nodiscard]] constexpr bool enabled() const noexcept {
    return on_start != nullptr || on_finish != nullptr;
  }
};

/// Connection-level lifecycle events for observability.
enum class connection_event_kind : std::uint8_t {
  connected = 1,
  disconnected,
  closed,
};

/// Connection event payload.
struct connection_event {
  connection_event_kind kind{connection_event_kind::connected};
  detail::connection_state state{detail::connection_state::INIT};

  // Monotonic successful-connect generation counter (increments on each OPEN transition).
  std::uint64_t generation{0};

  // Reconnection attempt counter used by backoff policy (0 for regular connected).
  int reconnect_count{0};

  // Error details for failure-related events.
  error_info error{};
};

/// Lightweight connection lifecycle hooks.
///
/// Threading / performance contract:
/// - Callback is invoked on the connection strand.
/// - Implementations MUST be non-blocking and MUST NOT throw.
struct connection_event_hooks {
  using on_event_fn = void (*)(void*, connection_event const&);

  void* user_data{};
  on_event_fn on_event{};

  [[nodiscard]] constexpr bool enabled() const noexcept { return on_event != nullptr; }
};

}  // namespace rediscoro
