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
/// - INIT -> CONNECTING: start() called, begin TCP connection
/// - CONNECTING -> OPEN: TCP connected, handshake complete
/// - CONNECTING -> FAILED: connection timeout or error
/// - OPEN -> CLOSING: stop() called (graceful shutdown)
/// - OPEN -> FAILED: IO error during operation
/// - FAILED -> RECONNECTING: immediate reconnect attempt (if enabled)
/// - FAILED -> FAILED: exponential backoff sleep between reconnects
/// - RECONNECTING -> OPEN: reconnection successful
/// - RECONNECTING -> FAILED: reconnection failed, retry
/// - CLOSING -> CLOSED: all pending requests completed, socket closed
/// - FAILED/RECONNECTING -> CLOSED: user cancel (stop() or destructor)
///
/// State properties and operation semantics:
///
/// INIT:
/// - Socket not connected
/// - enqueue(): REJECTED immediately with not_connected
/// - connect(): transitions to CONNECTING
/// - stop(): transitions to CLOSED immediately
///
/// CONNECTING:
/// - TCP connection in progress
/// - enqueue(): REJECTED immediately with not_connected
/// - stop(): transitions to CLOSING
/// - On success: transitions to OPEN
/// - On error: transitions to FAILED
///
/// OPEN:
/// - Connected and actively processing requests
/// - enqueue(): ACCEPTED (normal operation)
/// - stop(): transitions to CLOSING
/// - On IO error: transitions to FAILED
/// - read_loop / write_loop process IO concurrently (full-duplex) on the same strand
///
/// FAILED:
/// - Error occurred (IO error, timeout, handshake failure, etc.)
/// - enqueue(): REJECTED immediately with connection_lost
/// - All pending requests at time of error are failed via pipeline.clear_all()
/// - IO loops IMMEDIATELY stop normal IO (no drain, no further reads/writes)
/// - Socket closed
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
/// Transitions out of FAILED:
/// - If reconnection enabled: → RECONNECTING (after optional sleep)
/// - If reconnection disabled: → CLOSED (for deterministic cleanup)
/// - If user cancels: → CLOSED
///
/// RECONNECTING:
/// - Actively attempting to reconnect
/// - enqueue(): ACCEPTED and queued (unlimited queue)
/// - Attempts: TCP connect → handshake (HELLO/AUTH/SELECT/SETNAME)
/// - Success → OPEN (reconnect_count_ reset to 0)
/// - Failure → FAILED (reconnect_count_++, retry with backoff)
/// - Infinite retry loop (only stops on user cancel)
///
/// CLOSING:
/// - Graceful shutdown in progress
/// - enqueue(): REJECTED immediately with connection_closed
/// - write_loop flushes pending writes
/// - read_loop drains pending reads (optional, may be bounded by timeout)
/// - Then transitions to CLOSED
///
/// CLOSED:
/// - Terminal state, socket closed
/// - enqueue(): REJECTED immediately with connection_closed
/// - All resources released
/// - Cannot transition to any other state
/// - Object can be destroyed safely
///
/// State invariants (MUST hold at all times):
/// 1. OPEN and RECONNECTING accept new work
/// 2. FAILED and CLOSING reject new work immediately
/// 3. CLOSED is terminal (no transitions out)
/// 4. FAILED can transition to OPEN (via RECONNECTING)
/// 5. control_loop runs until CLOSED and owns state transitions
/// 6. Only one state transition per handle_error() call
///
/// Reconnection semantics:
/// - Automatic reconnection is SUPPORTED
/// - Request replay is NOT SUPPORTED
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
/// - Decide when to give up (call stop() or destroy client)
/// - Implement application-level retry for failed requests (if needed)
/// - Handle partial failures in multi-command transactions
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
