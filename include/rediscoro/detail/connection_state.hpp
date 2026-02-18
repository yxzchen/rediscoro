#pragma once

#include <cstdint>

namespace rediscoro::detail {

/// Connection lifecycle states used by `detail::connection`.
///
/// Implementation-facing enum (not a user-level contract). All transitions are serialized on the
/// connection strand and driven by `connection::{connect,close}`, `handle_error()`,
/// `control_loop()`, and `transition_to_closed()`.
///
/// High-level lifecycle:
///
///   INIT -> CONNECTING -> OPEN
///            |             |
///            | (fail)      | (runtime IO/protocol error)
///            v             v
///         CLOSING <----- FAILED <------+
///            |             |           |
///            v             | (backoff) |
///          CLOSED          +--------> RECONNECTING
///                             (connect attempt)
///
/// Key semantics (aligned with the current implementation):
///
/// - **Enqueue gating**: only `OPEN` accepts user work; all other states fail immediately.
///   - `INIT`/`CONNECTING` -> `client_errc::not_connected`
///   - `FAILED`/`RECONNECTING` -> `client_errc::connection_lost`
///   - `CLOSING`/`CLOSED` -> `client_errc::connection_closed`
///   There is **no request buffering** across connection generations.
///
/// - **Initial connect failures do NOT use `FAILED`**:
///   `FAILED` is reserved for runtime errors *after* reaching `OPEN`.
///   If `do_connect()` fails during `CONNECTING`, `connect()` unifies cleanup by calling
///   `close()` which transitions `CONNECTING -> CLOSING -> CLOSED`.
///
/// - **Automatic reconnection**:
///   On runtime error: `OPEN -> FAILED` (socket closed, pipeline cleared), then:
///   - if reconnection enabled: `FAILED` may stay `FAILED` during backoff sleep, then
///     `FAILED -> RECONNECTING -> (OPEN | FAILED)` in a loop.
///   - if reconnection disabled: `FAILED -> CLOSING` and the actor exits to `CLOSED`.
///   Backoff sleep is cancellation-aware (a `close()` interrupts it promptly).
///
/// - **Who writes `CLOSED` (single-writer rule)**:
///   Only `transition_to_closed()` sets `state_ = CLOSED` at actor shutdown.
///
/// - **Retry support**:
///   `CLOSED` is the end of a *connection actor* lifecycle; a subsequent `connect()` may
///   explicitly reset `CLOSED -> INIT` to retry and start a new actor instance.
enum class connection_state {
  INIT = 1,
  CONNECTING,
  OPEN,
  FAILED,        // Error occurred, may sleep before reconnect
  RECONNECTING,  // Actively attempting reconnection
  CLOSING,
  CLOSED
};

}  // namespace rediscoro::detail
