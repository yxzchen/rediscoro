#pragma once

#include <rediscoro/error_info.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <system_error>

namespace rediscoro {

/// Classify the origin of a request for tracing.
enum class request_kind : std::uint8_t {
  user = 0,
  handshake = 1,
};

[[nodiscard]] constexpr auto to_string(request_kind kind) noexcept -> char const* {
  switch (kind) {
    case request_kind::user:
      return "user";
    case request_kind::handshake:
      return "handshake";
    default:
      return "unknown";
  }
}

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

[[nodiscard]] constexpr auto to_string(connection_event_kind kind) noexcept -> char const* {
  switch (kind) {
    case connection_event_kind::connected:
      return "connected";
    case connection_event_kind::disconnected:
      return "disconnected";
    case connection_event_kind::closed:
      return "closed";
    default:
      return "unknown";
  }
}

enum class connection_event_stage : std::uint8_t {
  unknown = 0,
  connect,
  handshake,
  runtime_io,
  reconnect,
  close,
  actor,
};

[[nodiscard]] constexpr auto to_string(connection_event_stage stage) noexcept -> char const* {
  switch (stage) {
    case connection_event_stage::unknown:
      return "unknown";
    case connection_event_stage::connect:
      return "connect";
    case connection_event_stage::handshake:
      return "handshake";
    case connection_event_stage::runtime_io:
      return "runtime_io";
    case connection_event_stage::reconnect:
      return "reconnect";
    case connection_event_stage::close:
      return "close";
    case connection_event_stage::actor:
      return "actor";
    default:
      return "unknown";
  }
}

/// Connection event payload.
struct connection_event {
  connection_event_kind kind{connection_event_kind::connected};
  connection_event_stage stage{connection_event_stage::unknown};
  std::chrono::steady_clock::time_point timestamp{};

  // Monotonic successful-connect generation counter (increments on each OPEN transition).
  std::uint64_t generation{0};

  // Reconnection attempt counter used by backoff policy (0 for regular connected).
  int reconnect_count{0};

  // Optional state transition hint (detail::connection_state numeric value).
  std::optional<std::int32_t> from_state{};
  std::optional<std::int32_t> to_state{};

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
