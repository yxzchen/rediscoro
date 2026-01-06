#pragma once

#include <cstdint>

namespace rediscoro::detail {

/// Connection state machine with automatic reconnection.
///
/// State diagram:
///
///   INIT
///    |
///    v
///  CONNECTING -----> FAILED <------+
///    |                 |           |
///    v                 v           |
///   OPEN ---------> FAILED ------> RECONNECTING
///    |               (sleep)         |
///    v                 ^             |
///  CLOSING             |             |
///    |                 +-------------+
///    v                              (fail)
///  CLOSED <---------------------------+
///    ^
///    |
///  (cancel)
///
/// State transitions:
/// - INIT -> CONNECTING: connect() begins initial TCP+handshake
/// - CONNECTING -> OPEN: initial connection succeeded (TCP + handshake complete)
/// - CONNECTING -> CLOSING -> CLOSED: initial connection failed (connect() unifies cleanup via close())
/// - OPEN -> FAILED: runtime IO error occurred during normal operation (read/write path calls handle_error())
/// - OPEN -> CLOSING -> CLOSED: close() requested by user
/// - FAILED -> RECONNECTING: control_loop begins reconnection attempt (if enabled)
/// - FAILED -> FAILED: control_loop backoff sleep between reconnection attempts
/// - FAILED -> CLOSING -> CLOSED: close() requested OR reconnection disabled and deterministic cleanup is chosen
/// - RECONNECTING -> OPEN: reconnection succeeded (TCP + handshake complete)
/// - RECONNECTING -> FAILED: reconnection failed, retry with backoff
/// - RECONNECTING -> CLOSING -> CLOSED: close() requested by user
/// - CLOSING -> CLOSED: actor_loop shutdown completes (transition_to_closed())
///
/// State write authority (CRITICAL):
/// - Only transition_to_closed() is allowed to set state_ = CLOSED.
/// - close() transitions to CLOSING and then joins the background actor; the actor performs
///   transition_to_closed() exactly once.
/// - FAILED is reserved for runtime IO errors AFTER reaching OPEN; initial connect() failures
///   MUST NOT enter FAILED.
///
/// Enqueue policy invariant (IMPORTANT):
/// - Only OPEN accepts user work (enqueue() is accepted).
/// - All other states reject enqueue() immediately:
///   * INIT/CONNECTING: not_connected
///   * FAILED/RECONNECTING: connection_lost
///   * CLOSING/CLOSED: connection_closed
/// - Rationale: deterministic failure semantics + avoid buffering across connection generations.
///
/// Legal transition table + trigger source (who changes state):
///
/// - INIT -> CONNECTING:
///   - Trigger: connection::connect() (on connection strand)
///   - Condition: connect() called and not already connecting/open
///
/// - INIT -> CLOSING -> CLOSED:
///   - Trigger: connection::close() (on strand) + actor_loop/transition_to_closed()
///   - Condition: user calls close() before connect()
///
/// - CONNECTING -> OPEN:
///   - Trigger: connection::connect()/do_connect() (handshake path, on strand)
///   - Condition: TCP connect + RESP3 handshake succeeds
///
/// - CONNECTING -> CLOSING -> CLOSED:
///   - Trigger: connection::connect() failure path calls connection::close()
///   - Condition: resolve/connect/handshake failure OR user cancels during connect()
///
/// - OPEN -> FAILED:
///   - Trigger: read_loop/write_loop (runtime IO path) calls handle_error()
///   - Condition: socket read/write error, protocol error, etc. while OPEN
///
/// - OPEN -> CLOSING -> CLOSED:
///   - Trigger: connection::close() + actor_loop join
///   - Condition: user calls close()
///
/// - FAILED -> RECONNECTING:
///   - Trigger: control_loop
///   - Condition: reconnection enabled and an attempt is scheduled (immediate or after backoff)
///
/// - FAILED -> FAILED:
///   - Trigger: control_loop
///   - Condition: reconnection enabled and backoff sleep is in progress
///
/// - FAILED -> CLOSING -> CLOSED:
///   - Trigger: connection::close() OR control_loop chooses deterministic shutdown when reconnection disabled
///   - Condition: user closes OR reconnection disabled
///
/// - RECONNECTING -> OPEN:
///   - Trigger: control_loop/do_connect()
///   - Condition: reconnection succeeds (TCP + handshake)
///
/// - RECONNECTING -> FAILED:
///   - Trigger: control_loop/do_connect()
///   - Condition: reconnection attempt fails
///
/// - RECONNECTING -> CLOSING -> CLOSED:
///   - Trigger: connection::close() + actor_loop join
///   - Condition: user calls close() during reconnection
///
/// State properties and operation semantics:
///
/// INIT:
/// - Socket not connected
/// - enqueue(): REJECTED immediately with not_connected
/// - connect(): transitions to CONNECTING
/// - close(): transitions to CLOSING -> CLOSED
///
/// CONNECTING:
/// - TCP connection in progress
/// - enqueue(): REJECTED immediately with not_connected
/// - close(): transitions to CLOSING -> CLOSED (connect() observes cancellation and unifies cleanup)
/// - On success: transitions to OPEN
/// - On error: transitions to CLOSING -> CLOSED (initial connect failure MUST NOT enter FAILED)
///
/// OPEN:
/// - Connected and actively processing requests
/// - enqueue(): ACCEPTED (normal operation)
/// - close(): transitions to CLOSING -> CLOSED
/// - On IO error: transitions to FAILED
/// - read_loop / write_loop process IO concurrently (full-duplex) on the same strand
///
/// FAILED:
/// - Error occurred during normal operation AFTER reaching OPEN (runtime IO error)
/// - enqueue(): REJECTED immediately with connection_lost
/// - All pending requests at time of error are failed via pipeline.clear_all()
/// - IO loops IMMEDIATELY stop normal IO (no drain, no further reads/writes)
/// - Socket closed
///
/// Cancellation requirement during FAILED backoff (MUST IMPLEMENT):
/// - FAILED may include an exponential backoff sleep between reconnection attempts.
/// - close() MUST interrupt any backoff delay immediately (no waiting for sleep to expire).
/// - Therefore: all sleeps MUST be cancellation-aware (e.g. timer wait composed with cancel/wakeup).
///
/// FAILED has two sub-phases:
///
/// 1. Immediate reconnect phase (reconnect_count_ < immediate_attempts):
///    - Transitions instantly to RECONNECTING (no sleep)
///    - enqueue() requests during FAILED are rejected (瞬态, very short window)
///    - No backoff delay
///
/// 2. Backoff reconnect phase (reconnect_count_ >= immediate_attempts):
///    - Stays in FAILED while sleeping (exponential backoff)
///    - enqueue() requests during sleep are REJECTED
///    - After sleep → transitions to RECONNECTING
///    - This creates a deliberate window where requests fail
///
/// Note on transient window (IMPORTANT):
/// - There exists a small window where the connection is not usable and enqueue() fails:
///   FAILED (before transitioning to RECONNECTING) and the entire RECONNECTING phase.
///   This is by design.
///
/// Transitions out of FAILED:
/// - If reconnection enabled: → RECONNECTING (after optional sleep)
/// - If reconnection disabled: → CLOSED (for deterministic cleanup)
/// - If user cancels: → CLOSED
///
/// RECONNECTING:
/// - Actively attempting to reconnect
/// - enqueue(): REJECTED immediately with connection_lost
/// - Attempts: TCP connect → handshake (HELLO/AUTH/SELECT/SETNAME)
/// - Success → OPEN (reconnect_count_ reset to 0)
/// - Failure → FAILED (reconnect_count_++, retry with backoff)
/// - Infinite retry loop (only stops on user cancel)
///
/// CLOSING:
/// - Shutdown in progress
/// - enqueue(): REJECTED immediately with connection_closed
///
/// Phase-1 behavior (determinism-first):
/// - No flush guarantee (no best-effort drain).
/// - Immediately fail all pending work via pipeline.clear_all(connection_closed) and close the socket.
///
/// Future optional enhancement:
/// - A graceful shutdown mode MAY flush pending writes and/or drain pending reads with a bounded timeout.
/// - If implemented, it must be specified precisely (flush boundary, failure behavior, timeouts).
///
/// Then transitions to CLOSED (via transition_to_closed()).
///
/// CLOSED:
/// - Terminal state, socket closed
/// - enqueue(): REJECTED immediately with connection_closed
/// - All resources released
/// - Cannot transition to any other state
/// - Object can be destroyed safely
///
/// State invariants (MUST hold at all times):
/// 1. Only OPEN accepts new work
/// 2. FAILED and CLOSING reject new work immediately
/// 3. CLOSED is terminal (no transitions out)
/// 4. FAILED can transition to OPEN (via RECONNECTING)
/// 5. control_loop runs until CLOSED and owns state transitions
/// 6. Only one state transition per handle_error() call
/// 7. Only transition_to_closed() writes CLOSED (single writer)
///
/// Reconnection semantics:
/// - Automatic reconnection is SUPPORTED
/// - Request replay is NOT SUPPORTED
/// - Users should treat reconnection as transport recovery only, not application-level retry.
///
/// What happens on connection failure:
/// 1. All pending requests at time of error are failed immediately
/// 2. Connection automatically enters reconnection loop (if enabled)
/// 3. New requests during RECONNECTING are queued
/// 4. Reconnection succeeds → queued requests are processed
/// 5. Reconnection fails → retry indefinitely (infinite loop)
/// 6. User cancel → all queued requests fail, connection CLOSED
///
/// What is NOT supported:
/// - Request replay: Failed requests are NOT automatically retried
/// - Idempotent retry: User must implement their own retry logic
/// - Finite retry: Reconnection loops indefinitely (user decides when to give up)
///
/// Reconnection strategy:
/// - Immediate phase: N attempts with no delay (fast recovery)
/// - Backoff phase: Exponential delay, capped at max_delay
/// - Infinite loop: Never gives up automatically
///
/// Example scenario:
///   t=0: Connection breaks, 5 pending requests fail
///   t=0: Attempt 1-5 (immediate, no delay)
///   t=0: Attempt 6+ (exponential backoff: 100ms, 200ms, 400ms, ...)
///   t=X: Delay reaches max (30s), keep retrying every 30s
///   t=Y: User enqueues new request → queued
///   t=Z: Reconnection succeeds → queued request processed
///
/// User responsibility:
/// - Decide when to give up (call close() or destroy client)
/// - Implement application-level retry for failed requests (if needed)
/// - Handle partial failures in multi-command transactions
///
/// Enum ordering note:
/// - The enum numeric ordering has no semantic meaning; transitions are explicitly controlled.
enum class connection_state{
  INIT = 1,
  CONNECTING,
  OPEN,
  FAILED,       // Error occurred, may sleep before reconnect
  RECONNECTING, // Actively attempting reconnection
  CLOSING,
  CLOSED
};

}  // namespace rediscoro::detail
