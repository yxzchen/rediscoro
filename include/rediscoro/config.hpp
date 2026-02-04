#pragma once

#include <rediscoro/tracing.hpp>

#include <chrono>
#include <string>

namespace rediscoro {

/// Reconnection policy configuration.
///
/// Strategy:
/// 1. Immediate reconnection (no delay):
///    - First `immediate_attempts` reconnections happen instantly
///    - No state window where requests would be rejected
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
  /// During this phase, there's no window where requests are rejected.
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
};

/// Client configuration.
struct config {
  // Connection parameters
  std::string host = "localhost";
  int port = 6379;

  // Timeouts
  /// DNS/host resolution timeout (getaddrinfo on a background thread).
  ///
  /// Notes:
  /// - iocoro resolver does NOT support cancellation; a timed-out resolve may still finish
  ///   in the background (result will be ignored).
  /// - close()/cancel can wake connect() promptly, but cannot stop the underlying getaddrinfo.
  std::chrono::milliseconds resolve_timeout{5000};
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds request_timeout{5000};

  // Authentication & setup
  std::string username{};
  std::string password{};
  int database = 0;
  std::string client_name{};

  // Reconnection behavior
  reconnection_policy reconnection{};

  // Tracing hooks (request-level instrumentation).
  request_trace_hooks trace_hooks{};

  // Whether to emit tracing events for the initial handshake (HELLO/AUTH/SELECT/SETNAME).
  // Default off to avoid noise.
  bool trace_handshake{false};
};

}  // namespace rediscoro
