#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
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

}  // namespace rediscoro
