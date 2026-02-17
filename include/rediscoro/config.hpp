#pragma once

#include <rediscoro/tracing.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace rediscoro {

/// Reconnection policy configuration.
///
/// Strategy:
/// 1. Immediate reconnection (no delay):
///    - First `immediate_attempts` reconnections happen instantly
///    - A reject window may still appear during FAILED/RECONNECTING transitions
///
/// 2. Backoff reconnection (with exponential delay):
///    - After immediate attempts exhausted, start exponential backoff
///    - Delay = initial_delay * (backoff_factor ^ attempt_number)
///    - Capped at max_delay
///    - During sleep, state = FAILED, new requests are rejected
///
/// 3. Infinite retry:
///    - Never gives up automatically
///    - Keeps retrying with max_delay indefinitely
///    - Only stops on user cancel (stop() or destructor)
struct reconnection_policy {
  /// Enable automatic reconnection.
  /// If false, connection enters CLOSED on error (no retry).
  bool enabled = true;

  /// Number of immediate reconnection attempts (no delay).
  /// During this phase, a reject window may still appear during state transitions.
  /// Recommended: 5-10 attempts.
  int immediate_attempts = 5;

  /// Initial delay for backoff phase (after immediate attempts).
  std::chrono::milliseconds initial_delay{100};

  /// Maximum delay between reconnection attempts.
  /// Once reached, keeps retrying at this interval indefinitely.
  std::chrono::milliseconds max_delay{30000};

  /// Exponential backoff factor.
  /// delay = initial_delay * (backoff_factor ^ attempt_number)
  double backoff_factor = 2.0;

  /// Relative random jitter applied to delayed retries (0.2 => +/-20%).
  /// Jitter is ignored for immediate (zero-delay) retries.
  double jitter_ratio = 0.2;
};

/// Client configuration.
struct config {
  // Connection parameters
  std::string host = "localhost";
  int port = 6379;

  // Timeouts (optional - nullopt means no timeout)
  /// DNS/host resolution timeout (getaddrinfo on a background thread).
  ///
  /// Notes:
  /// - iocoro resolver does NOT support cancellation; a timed-out resolve may still finish
  ///   in the background (result will be ignored).
  /// - close()/cancel can wake connect() promptly, but cannot stop the underlying getaddrinfo.
  std::optional<std::chrono::milliseconds> resolve_timeout{5000};

  /// TCP connection timeout.
  std::optional<std::chrono::milliseconds> connect_timeout{5000};

  /// Request timeout (per-request deadline).
  /// If nullopt, no timeout is applied (indefinite wait).
  std::optional<std::chrono::milliseconds> request_timeout{5000};

  // RESP3 input hardening limits (enabled by default).
  // Exceeding these limits is treated as protocol_errc::invalid_length.
  std::size_t max_resp_bulk_bytes = 512ULL * 1024ULL * 1024ULL;  // 512 MiB
  std::uint32_t max_resp_container_len = 1'000'000U;
  std::size_t max_resp_line_bytes = 64ULL * 1024ULL;  // 64 KiB

  // Pipeline backpressure limits (enabled by default).
  // Exceeding either limit causes fast-fail with client_errc::queue_full.
  std::size_t max_pipeline_requests = 16'384U;
  std::size_t max_pipeline_pending_write_bytes = 64ULL * 1024ULL * 1024ULL;  // 64 MiB

  // Authentication & setup
  std::string username{};
  std::string password{};
  int database = 0;
  std::string client_name{};

  // Reconnection behavior
  reconnection_policy reconnection{};

  // Tracing hooks (request-level instrumentation).
  request_trace_hooks trace_hooks{};
  // Redact request trace error detail by default (detail = "").
  bool trace_redact_error_detail{true};

  // Connection lifecycle hooks (connected/disconnected/closed instrumentation).
  connection_event_hooks connection_hooks{};

  // Whether to emit tracing events for the initial handshake (HELLO/AUTH/SELECT/SETNAME).
  // Default off to avoid noise.
  bool trace_handshake{false};
};

}  // namespace rediscoro
