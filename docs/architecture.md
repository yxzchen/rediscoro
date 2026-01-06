# rediscoro Connection Architecture

## Overview

This document describes the connection management architecture for rediscoro, a coroutine-based Redis client.

## Core Principles

1. **Coroutine-only API**: All async operations return `iocoro::awaitable<T>`
2. **Executor binding**: Each coroutine is structurally bound to an executor
3. **Single ownership**: Socket is owned by exactly one coroutine (worker_loop)
4. **Actor model**: Connection is a long-running coroutine actor
5. **No callbacks**: All communication via message passing and awaitable results

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
- `execute<T>(...)` - Execute single command

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

  while (state_ == OPEN) {
    co_await wakeup_.wait();

    if (cancel_.is_cancelled()) {
      state_ = CLOSED;
      break;
    }

    if (pipeline_.has_pending_write()) {
      co_await do_write();
    }

    if (pipeline_.has_pending_read()) {
      co_await do_read();
    }
  }

  transition_to_closed();
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
  CONNECTING -----> FAILED
    |                 |
    v                 |
   OPEN -----------> FAILED
    |                 |
    v                 |
  CLOSING             |
    |                 |
    v                 v
              CLOSED
```

**States:**
- `INIT` - Initial state, not connected
- `CONNECTING` - TCP connection in progress
- `OPEN` - Connected and ready
- `FAILED` - Unrecoverable error
- `CLOSING` - Graceful shutdown
- `CLOSED` - Terminal state

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
- Store response value or error
- Provide awaitable interface
- Single-shot notification

**Constraints:**
- Can only be awaited once
- Can only be set once
- Thread-safe (for cross-executor usage)

**Key Methods:**
- `set_value(T)` - Set successful response
- `set_error(error)` - Set error response
- `wait()` - Await completion

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
- Pipeline operates on `response_sink*`, not `pending_response<T>*`
- Type system prevents pipeline from accessing coroutine internals
- Impossible to accidentally inline user code

**Implementation:**
- `pending_response<T>` implements `response_sink`
- `deliver()` adapts message to `T` and notifies
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
client.execute<T>(...)
  |
  v
request created
  |
  v
connection.enqueue<T>(request)
  |
  v
pending_response<T> created
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
pending_response.set_value(T) or set_error(error)
  |
  v
notify_event.notify()
  |
  v
client coroutine resumes
  |
  v
return response_slot<T>
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
**Invariant:** Pipeline never knows about coroutines or pending_response<T>

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
).execute<std::string, ignore_t, int64_t>();
```

### Pub/Sub Support
- Separate push message handler
- Out-of-band message dispatch
- Subscribe/unsubscribe commands

### Reconnection (NOT CURRENTLY SUPPORTED)

**Current behavior:**
- Connection enters FAILED → CLOSED on error
- All pending requests fail
- No automatic reconnection
- No request replay

**Why deferred:**
- Adds significant complexity to state machine
- Requires request replay buffer
- Idempotency concerns (not all Redis commands safe to replay)
- Memory management for replay buffer

**If implemented in future:**
- New `RECONNECTING` state
- Separate `reconnection_policy` component
- Opt-in request replay with idempotency tags
- Exponential backoff
- Max retry limits

**Current workaround:**
- Client layer detects failure
- Creates new connection
- Manually retries safe operations

## Implementation Status

✅ Framework and structure defined
⏳ Implementation details pending
⏳ Unit tests pending
⏳ Integration tests pending
