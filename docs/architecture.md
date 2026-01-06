# rediscoro Connection Architecture

## Overview

This document describes the connection management architecture for rediscoro, a coroutine-based Redis client.

## Core Principles

1. **Coroutine-only API**: All async operations return `iocoro::awaitable<T>`
2. **Executor binding**: Each coroutine is structurally bound to an executor
3. **Single ownership**: Socket is owned by exactly one coroutine (worker_loop)
4. **Actor model**: Connection is a long-running coroutine actor
5. **No callbacks**: All communication via message passing and awaitable results
6. **No replay / no retry promise**: This design does NOT guarantee semantic equivalence across connection failures and provides NO automatic retry or request replay.
7. **IO executor only**: `client` / `connection` are constructed with `iocoro::io_executor` (the only executor that can drive socket IO).

## Module Structure

```
client
 ├── client.hpp / client.ipp
 ├── config.hpp
 ├── request.hpp
 ├── response.hpp
 └── detail/
      ├── connection_state.hpp      - State machine definitions
      ├── connection.hpp / ipp       - Core actor implementation
      ├── pipeline.hpp / ipp         - Request/response scheduling
      ├── pending_response.hpp / ipp - Async response awaiter
      ├── notify_event.hpp / ipp     - Coroutine notification primitive
      ├── executor_guard.hpp / ipp   - Strand wrapper
      └── cancel_source.hpp          - Cancellation support
```

## Key Components

### 1. client

**Responsibilities:**
- Manage connection lifecycle
- Provide user-facing async API
- Create and forward requests to connection

**Key Methods:**
- `connect()` - Connect to Redis server
- `close()` - Graceful shutdown
- `exec<T>(...)` - Execute single command (returns `response<T>` size 1)
- `exec<Ts...>(request)` - Execute fixed-size pipeline (returns `response<Ts...>`)
- `exec_dynamic<T>(request)` - Execute runtime-size pipeline (returns `dynamic_response<T>`)

**NOT Responsible For:**
- IO operations
- Protocol parsing
- Pipeline management

### 2. connection (Core Actor)

**Responsibilities:**
- Run worker_loop coroutine on a single strand
- Manage socket lifecycle
- Serialize all socket operations
- Dispatch incoming responses

**Structural Constraints:**
- Only one worker_loop coroutine
- Socket only accessed within worker_loop
- All external requests enqueued via thread-safe methods
- Worker loop runs until CLOSED state

**Key Methods:**
- `start()` - Spawn worker_loop
- `stop()` - Request graceful shutdown
- `enqueue<T>(request)` - Queue request for execution
- `worker_loop()` - Main event loop

**Internal Helpers:**
- `do_connect()` - TCP connection with timeout/retry
- `do_read()` - Read and parse RESP3 messages
- `do_write()` - Write pending requests
- `do_auth()`, `do_select_db()`, `do_set_client_name()` - Handshake

### 3. worker_loop Structure

```cpp
awaitable<void> connection::worker_loop() {
  co_await do_connect();

  while (!cancel_.is_cancelled() && state_ != CLOSED) {
    co_await wakeup_.wait();

    if (cancel_.is_cancelled()) {
      break;
    }

    if (state_ == FAILED) {
      // Backoff sleep may happen here (state stays FAILED during sleep),
      // then transition to RECONNECTING and attempt reconnection.
      co_await do_reconnect();
      continue;
    }

    if (pipeline_.has_pending_write()) {
      co_await do_write();
    }

    if (pipeline_.has_pending_read()) {
      co_await do_read();
    }
  }

  transition_to_closed();  // stop() waits until CLOSED
}
```

**Key Properties:**
- No select/poll - explicit control flow
- No concurrent reads/writes
- No handler callbacks
- Explicit state transitions

### 4. Connection State Machine

```
   INIT
    |
    v
  CONNECTING -----> FAILED <------+
    |                 |           |
    v                 v           |
   OPEN ---------> FAILED ------> RECONNECTING
    |               (sleep)         |
    v                 ^             |
  CLOSING             |             |
    |                 +-------------+
    v                              (fail)
  CLOSED <---------------------------+
    ^
    |
  (cancel)
```

**States:**
- `INIT` - Initial state, not connected
- `CONNECTING` - TCP connection in progress
- `OPEN` - Connected and ready
- `FAILED` - Error occurred, or waiting in backoff sleep before next reconnect attempt
- `RECONNECTING` - Actively attempting to reconnect (TCP connect + handshake)
- `CLOSING` - Graceful shutdown
- `CLOSED` - Terminal state

**Backoff window semantics (重要):**
- `FAILED → (sleep delay) → RECONNECTING`
- During the sleep delay, state remains `FAILED` (no extra "waiting" substate).
- `FAILED` rejects new enqueue() (this is the deliberate "window where requests fail").
- `RECONNECTING` accepts new enqueue() and queues requests (no request replay for already-failed requests).

### 5. pipeline

**Responsibilities:**
- Maintain request FIFO ordering
- Track pending writes and reads
- Dispatch RESP3 messages to pending responses

**Key Operations:**
- `push(request, slot)` - Enqueue request
- `has_pending_write/read()` - Check pending operations
- `next_write_buffer()` - Get data to write
- `on_write_done(n)` - Update write progress
- `on_message(msg)` - Dispatch response

**NOT Responsible For:**
- IO operations
- Executor management
- Resuming coroutines (done by pending_response)

### 6. pending_response\<T\>

**Responsibilities:**
- Aggregate one or more replies into `response<Ts...>` or `dynamic_response<T>`
- Provide awaitable interface (`wait()`)
- Notify exactly once when the aggregate is complete

**Constraints:**
- deliver() is strand-only and may be called multiple times until completion
- wait() resumes on the awaiting coroutine's executor via notify_event

**Key Methods:**
- `deliver(resp3::message)` - Consume one reply
- `deliver_error(resp3::error)` - Consume one reply as error
- `wait()` - Await completion, returning `response<...>` / `dynamic_response<T>`

### 7. notify_event

**Responsibilities:**
- **Counting** coroutine notification (NOT single-shot)
- Resume on original executor
- Thread-safe notify
- No signal loss

**Key Methods:**
- `wait()` - Suspend until count > 0, consumes one count
- `notify()` - Increment count, resume if waiting

**Critical Properties:**
- **Counting semantics:** Multiple notify() = multiple wakeups
- Resume MUST happen on the awaiting coroutine's executor
- Lock-free check via atomic counter
- Mutex only for coroutine handle protection
- **Atomicity boundary (MUST):** wait() MUST perform "consume-or-register" as one atomic decision:
  - either consume one count and return without suspending, OR
  - register waiter (handle + executor) and suspend
  Count checking, waiter registration, and suspend decision MUST NOT be separated.

**Why counting:**
```cpp
// Without counting:
enqueue(req1);  // notify()
enqueue(req2);  // notify() - LOST if worker already woke
// Result: req2 never sent

// With counting:
enqueue(req1);  // count = 1
enqueue(req2);  // count = 2
worker waits;   // count = 1 (consumed one)
worker drains;  // processes both requests
worker waits;   // count = 0 (consumed one)
```

### 8. response_sink (Abstract Interface)

**Purpose:**
- Type-erase response delivery
- Prevent pipeline from knowing about coroutines
- Enforce "no user code in worker loop" invariant

**Interface:**
```cpp
class response_sink {
  virtual void deliver(resp3::message) = 0;
  virtual void deliver_error(resp3::error) = 0;
  virtual bool is_complete() const = 0;
};
```

**Why This Matters:**
- Pipeline operates on `response_sink*`, not coroutine-aware types
- Type system prevents pipeline from accessing coroutine internals
- Impossible to accidentally inline user code

**Implementation:**
- `pending_response<Ts...>` implements `response_sink` for fixed-size pipelines
- `pending_dynamic_response<T>` implements `response_sink` for runtime-sized pipelines
- `deliver()` aggregates replies into `response<Ts...>` / `dynamic_response<T>` and notifies
- Called ONLY from connection strand (simplified thread-safety)

### 8. executor_guard

**Responsibilities:**
- Wrap executor with strand
- Ensure serialized access to connection state
- Provide stable executor reference

**Why Strand:**
- Prevents concurrent access to socket
- Serializes all connection operations
- Simplifies reasoning about state transitions

## Data Flow

### Request Path

```
client.exec<T>(...)
  |
  v
request created
  |
  v
connection.enqueue<T>(request)
  |
  v
pending_response<T> created  (N=1 fixed-size response)
  |
  v
pipeline.push(request, pending_response)
  |
  v
wakeup_.notify()
  |
  v
worker_loop wakes up
  |
  v
do_write() - send to socket
  |
  v
request moves to awaiting_read queue
  |
  v
client awaits pending_response.wait()
```

### Response Path

```
worker_loop
  |
  v
do_read() - receive from socket
  |
  v
parser.parse_one()
  |
  v
pipeline.on_message(msg)
  |
  v
adapter::adapt<T>(msg)
  |
  v
pending_response aggregates reply into response<T> slot (or error)
  |
  v
notify_event.notify()
  |
  v
client coroutine resumes
  |
  v
return response<T> (size 1)
```

## Thread Safety Model

### Cross-Executor Safe (Rare)
- `connection::enqueue()` - Can be called from any executor
  - Posts work to strand via notify
  - Does NOT touch connection state directly
- `notify_event::notify()` - Can be called from any thread
  - Atomic increment + executor post
- `cancel_source::request_cancel()` - Can be called from any thread
  - Atomic store

### Strand-Only (Majority)
- All `worker_loop` internals
- All `pipeline` methods
- All `response_sink::deliver()` calls
- Socket operations
- State transitions
- `enqueue_impl()` (internal, called after posting to strand)

### Simplified Thread-Safety Model

**Key simplification from review:**
- `pending_response::deliver()` is strand-only
- No cross-executor synchronization needed
- Only `notify_event` handles executor dispatch

**Why this is safe:**
- Pipeline runs on strand
- Pipeline is the only caller of deliver()
- No concurrent deliver() possible
- wait() reads result AFTER notification

**Contrast with complex model:**
```cpp
// COMPLEX (original): deliver() from any thread
void deliver(msg) {
  std::lock_guard lock(mutex_);  // Need lock!
  result_ = adapt(msg);
  event_.notify();  // Cross-executor resume
}

// SIMPLE (current): deliver() from strand only
void deliver(msg) {
  // No lock needed - single-threaded
  result_ = adapt(msg);
  event_.notify();  // Cross-executor resume
}
```

## Structural Invariants (CRITICAL - DO NOT VIOLATE)

These are not "best practices" - they are system-level invariants that, if broken, cause data races or deadlocks:

### 1. Socket Single Ownership
**Invariant:** Socket is owned by exactly one coroutine (worker_loop)

**Enforcement:**
- Only worker_loop accesses socket
- No concurrent reads/writes
- do_read/do_write are subroutines, NOT spawned coroutines
- executor_guard prohibits nested co_spawn

**Violation consequences:**
- Data race on socket state
- Corrupted RESP3 protocol stream
- Undefined behavior

### 2. Executor-Correct Resumption
**Invariant:** Coroutine MUST resume on its original executor

**Enforcement:**
- notify_event captures executor at wait() time
- notify() posts resumption to captured executor
- Never inline resume in notify()

**Violation consequences:**
- User code runs on wrong executor
- Breaks user's concurrency assumptions
- Possible deadlocks or race conditions

### 3. No User Code in Worker Loop
**Invariant:** Worker loop never executes user-provided code

**Enforcement:**
- Pipeline operates on abstract response_sink interface
- adapter::adapt<T> restricted to standard/trivial types
- pending_response::deliver() only stores and notifies

**Violation consequences:**
- User code runs on connection strand
- Can block worker loop (e.g., logging, locks)
- Breaks isolation between connection and user logic

### 4. Pipeline Type Isolation
**Invariant:** Pipeline never knows about coroutines or pending_response types

**Enforcement:**
- Pipeline operates ONLY on response_sink* (abstract)
- Cannot access coroutine_handle or executor
- Type system prevents accidental coupling

**Violation consequences:**
- Pipeline could accidentally resume coroutines
- Violates invariant #3
- Breaks layering

### 5. Actor Model (Not Mutex+Socket)
**Invariant:** Connection is an actor with single-threaded logic

**Enforcement:**
- All state mutations on connection strand
- No locks (except notify_event internals)
- enqueue() posts to worker loop, doesn't touch state directly

**Violation consequences:**
- Need locks everywhere
- Deadlock potential
- Loss of reasoning clarity

### 6. Counting Notification (No Signal Loss)
**Invariant:** notify() ALWAYS results in wakeup (eventually)

**Enforcement:**
- notify_event uses atomic counter
- Multiple notify() = multiple wakeups
- Worker loop drains all work before sleeping

**Violation consequences:**
- "Work queued but worker sleeping" deadlock
- Requests never complete
- Silent hang (hardest bug to debug)

## Error Handling

### Connection Errors
- IO errors -> `FAILED` state
- Parse errors -> passed to pending_response
- Timeout -> `FAILED` state (with retry logic in do_connect)

**FAILED behavior (MUST):**
- Once `state` becomes `FAILED`, the worker loop MUST NOT perform any further normal socket IO (`do_read()` / `do_write()`).
- After FAILED: only in-memory cleanup + error delivery is allowed, then optional reconnection attempts (TCP connect + handshake) via `do_reconnect()`.

### Request Errors
- Redis errors (simple_error, bulk_error) -> `response_error`
- Adapter errors (type mismatch) -> `response_error`
- Connection closed -> all pending requests get error

### Cancellation
- `cancel_source::request_cancel()` signals worker_loop
- Worker loop transitions to `CLOSED`
- All pending requests get cancellation error

## Future Extensions

### Pipeline API
```cpp
auto [r1, r2, r3] = co_await client.pipeline(
  request{"GET", "key1"},
  request{"SET", "key2", "value"},
  request{"INCR", "counter"}
).exec<std::string, ignore_t, int64_t>();
```

### Pub/Sub Support
- Separate push message handler
- Out-of-band message dispatch
- Subscribe/unsubscribe commands

### Reconnection (SUPPORTED, no request replay)

**Current behavior:**
- Connection enters `FAILED` on IO/handshake error
- All pending in-flight requests at the moment of error fail immediately (no replay)
- If reconnection is enabled, connection keeps trying to reconnect indefinitely until success or user cancel
- New requests during `RECONNECTING` are queued; during `FAILED` (backoff sleep window) are rejected

**When reconnection is disabled:**
- The connection still transitions to `CLOSED` after failure to guarantee deterministic cleanup and make `stop()` semantics uniform.
- The failure reason is preserved for diagnostics (see connection's last_error concept).

**State semantics during reconnection:**
- `FAILED`: either a very short transient between attempts, or the deliberate backoff sleep window; enqueue() rejects
- `RECONNECTING`: actively attempting TCP connect + handshake; enqueue() accepts and queues
- On successful reconnection: `state = OPEN` and `reconnect_count_` is reset to 0

**do_reconnect() return semantics:**
- do_reconnect() does not "fail return": it loops until either success (returns with `state = OPEN`) or user cancel (returns with `state = CLOSED`).

**Error de-duplication (重要):**
- Multiple IO paths may report errors close together (e.g., read and write both observe a broken socket).
- `handle_error()` must be idempotent / guarded so reconnection is initiated only once.

**Not supported:**
- Request replay / automatic retry of already-failed requests
- Command idempotency tagging for safe replay

## Implementation Status

✅ Framework and structure defined
⏳ Implementation details pending
⏳ Unit tests pending
⏳ Integration tests pending
