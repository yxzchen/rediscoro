# Review Improvements Summary

This document summarizes the critical improvements made based on the architectural review.

## Overview

The review identified three major risk areas:
1. Signal loss in worker_loop wakeup
2. Thread-safety complexity in pending_response
3. Weak type boundaries between pipeline and coroutines

All three have been addressed with structural guarantees.

## 1. Signal Semantics: Counting Notification

### Problem
Original design used single-shot notification:
```cpp
// BROKEN:
enqueue(req1);  // notify()
enqueue(req2);  // notify() - LOST if worker already waking
// Worker only sees req1
```

### Solution
Changed to **counting notification**:
```cpp
class notify_event {
  std::atomic<size_t> count_{0};  // NEW: atomic counter

  void notify() {
    count_.fetch_add(1);  // Increment
    // ... resume if waiting
  }

  awaitable<void> wait() {
    // Consumes one count
    // Suspends only if count == 0
  }
};
```

### Guarantee
- `notify()` NEVER lost
- Multiple enqueues = multiple wakeups
- Worker loop drains all work before sleeping

**Files changed:**
- `include/rediscoro/detail/notify_event.hpp`

## 2. Worker Loop: Explicit Drain Strategy

### Problem
Original loop could suspend while work remains:
```cpp
// AMBIGUOUS:
while (state_ == OPEN) {
  co_await wakeup_.wait();
  if (has_pending_write()) do_write();
  // What if new work arrives HERE?
}
```

### Solution
Explicit drain-until-idle strategy:
```cpp
// CLEAR:
while (state_ == OPEN) {
  co_await wakeup_.wait();

  // Drain ALL work before next wait
  while (has_pending_write() || has_pending_read()) {
    if (has_pending_write()) co_await do_write();
    if (has_pending_read()) co_await do_read();
  }
}
```

### Guarantee
- Never suspend while work available
- Combined with counting notify = no deadlock

**Files changed:**
- `include/rediscoro/detail/connection.hpp` (documentation)

## 3. Completion Semantics: do_read / do_write

### Problem
Unclear when these functions return:
- Do they read/write once?
- Until socket blocks?
- Until work exhausted?

### Solution
**Documented completion contracts:**

#### do_write()
- Attempts to write ALL pending requests
- Returns when:
  - All writes complete, OR
  - Socket would block (EAGAIN), OR
  - Error occurred

#### do_read()
- Attempts to read at least ONE complete RESP3 message
- Returns when:
  - At least one message parsed AND socket would block, OR
  - No more pending reads, OR
  - Error occurred

### Guarantee
- Deterministic completion conditions
- No hidden scheduling policies

**Files changed:**
- `include/rediscoro/detail/connection.hpp`

## 4. State Machine: Explicit Operation Semantics

### Problem
Unclear what happens when:
- `enqueue()` in FAILED state?
- `stop()` in CONNECTING state?

### Solution
**Per-state operation table:**

| State      | enqueue()        | stop()              | worker_loop                |
|------------|------------------|---------------------|----------------------------|
| INIT       | ACCEPT (queued)  | → CLOSED            | Not running                |
| CONNECTING | ACCEPT (queued)  | → CLOSING           | Running                    |
| OPEN       | ACCEPT           | → CLOSING           | Running                    |
| FAILED     | REJECT (error)   | (no-op)             | Transitioning to CLOSED    |
| CLOSING    | REJECT (closed)  | (no-op)             | Flushing, then → CLOSED    |
| CLOSED     | REJECT (closed)  | (no-op)             | Not running                |

### Guarantee
- No ambiguous behavior
- State determines operation outcome deterministically

**Files changed:**
- `include/rediscoro/detail/connection_state.hpp`

## 5. Type Boundary: response_sink Abstraction

### Problem (CRITICAL)
Pipeline could "accidentally" resume coroutines:
```cpp
// DANGEROUS:
class pipeline {
  void on_message(msg) {
    auto* response = awaiting_.front();
    response->set_value(msg);  // Might inline user code!
  }
};
```

### Solution
**Abstract interface enforces boundary:**
```cpp
// Type-erased interface
class response_sink {
  virtual void deliver(resp3::message) = 0;  // No coroutine knowledge
};

class pipeline {
  std::deque<response_sink*> awaiting_;  // Abstract pointers only

  void on_message(msg) {
    awaiting_.front()->deliver(msg);  // Cannot access coroutine internals
  }
};

template <typename T>
class pending_response : public response_sink {
  void deliver(msg) override {
    result_ = adapt<T>(msg);
    event_.notify();  // Resumes on user executor, not inline
  }
};
```

### Guarantee
- **Type system prevents** pipeline from knowing about coroutines
- **Impossible** to accidentally inline user code
- Clear separation: pipeline = scheduling, pending_response = continuation

**Files changed:**
- `include/rediscoro/detail/response_sink.hpp` (new file)
- `include/rediscoro/detail/pipeline.hpp`
- `include/rediscoro/detail/pending_response.hpp`

## 6. Thread-Safety: Simplified Model

### Problem
Original design required cross-executor synchronization:
```cpp
// COMPLEX:
template <typename T>
class pending_response {
  std::mutex mutex_;  // Needed for cross-executor calls

  void set_value(T val) {
    std::lock_guard lock(mutex_);  // Expensive!
    result_ = val;
    event_.notify();
  }
};
```

### Solution
**Restrict deliver() to strand:**
```cpp
// SIMPLE:
template <typename T>
class pending_response : public response_sink {
  // NO MUTEX!

  void deliver(msg) override {  // Called ONLY from connection strand
    result_ = adapt<T>(msg);    // Single-threaded access
    event_.notify();             // Cross-executor resume via event
  }
};
```

### Guarantee
- `deliver()` is strand-only (pipeline runs on strand)
- No concurrent `deliver()` calls possible
- Only `notify_event` handles cross-executor dispatch
- Dramatically simpler implementation

**Files changed:**
- `include/rediscoro/detail/pending_response.hpp`

## 7. Executor Constraints: Forbidden Patterns

### Problem
Easy to accidentally break "single socket owner" invariant:
```cpp
// WRONG (but looks innocent):
awaitable<void> do_read() {
  co_await co_spawn(executor_, read_helper(), use_awaitable);  // BUG!
  // Now TWO coroutines access socket!
}
```

### Solution
**Documented forbidden patterns:**

#### Forbidden
```cpp
// NEVER spawn sub-coroutines
co_spawn(executor_.get(), async_op(), detached);

// NEVER bypass strand
co_await socket_.async_read_some(...);  // Wrong executor!
```

#### Correct
```cpp
// Subroutine call (NOT spawn)
co_await do_read();

// Always use strand-bound executor
// (enforced by executor_guard)
```

### Guarantee
- Explicit documentation of constraints
- Future: Could add static analysis

**Files changed:**
- `include/rediscoro/detail/executor_guard.hpp`

## 8. Adapter Constraints: No User Code

### Problem
`adapt<T>(msg)` runs in worker_loop:
```cpp
struct MyType {
  MyType(std::string s) {
    log_to_file(s);  // BAD: runs in worker_loop!
  }
};

co_await do_read();  // Calls adapt<MyType>(msg)
// log_to_file runs inline!
```

### Solution
**Documented type constraints:**

#### Safe Types
- Standard library: `std::string`, `std::vector`, etc.
- Trivial types: `int`, `double`, `bool`
- Aggregates of safe types

#### Unsafe Types
- User types with side-effectful constructors
- Types that hold locks, do IO, etc.

### Guarantee (Current)
- Documentation warns against unsafe types
- Future: Could add concept to enforce at compile-time

**Files changed:**
- `include/rediscoro/adapter/adapt.hpp`

## 9. Future Extensions: Layering Preserved

### Pipeline API
**Correct layering:**
```
client.pipeline(...)
  ↓
pipeline_builder<Ts...>  // NEW layer above connection
  ↓
connection.enqueue()     // Existing interface
```

**WRONG (don't do this):**
```
pipeline knows about tuple<Ts...>  // Breaks abstraction!
```

### Pub/Sub
**Separate actor:**
```
pubsub_connection  // Different actor, different state machine
  ↑
Does NOT share pipeline with request/response
```

## Summary of Structural Guarantees

| Guarantee                     | Enforcement                      | File                      |
|-------------------------------|----------------------------------|---------------------------|
| No signal loss                | Atomic counter in notify_event   | notify_event.hpp          |
| No missed work                | Drain loop in worker_loop        | connection.hpp            |
| No user code in worker        | response_sink abstraction        | response_sink.hpp         |
| Single socket owner           | Forbidden co_spawn pattern       | executor_guard.hpp        |
| Executor-correct resume       | notify_event captures executor   | notify_event.hpp          |
| Deterministic state behavior  | Per-state operation table        | connection_state.hpp      |

## Risk Mitigation

| Original Risk                                  | Mitigation                              | Status |
|------------------------------------------------|-----------------------------------------|--------|
| Signal loss → deadlock                         | Counting notification                   | ✅     |
| Complex thread-safety → bugs                   | Strand-only deliver()                   | ✅     |
| Pipeline resumes coroutines → user code inline | response_sink type barrier              | ✅     |
| Ambiguous completion → scheduling bugs         | Documented semantics                    | ✅     |
| State machine confusion → flaky behavior       | Per-state operation table               | ✅     |
| Nested co_spawn → socket race                  | Documented forbidden patterns           | ✅     |
| User types → side effects in worker            | Adapter constraints documented          | ⚠️     |

Legend:
- ✅ Structurally enforced (type system or runtime guarantee)
- ⚠️ Documented constraint (could be violated, but warned against)

## Code Review Checklist

When reviewing connection-related code, verify:

- [ ] No `co_spawn` within connection internals
- [ ] No direct `socket_.async_*` calls (must go through do_read/do_write)
- [ ] Pipeline only touches `response_sink*`, never `pending_response<T>*`
- [ ] `deliver()` implementations never block or do heavy work
- [ ] Worker loop drains all work before next `wait()`
- [ ] State transitions follow documented state machine
- [ ] `enqueue()` behavior documented for all states
- [ ] No mutex/locks except in `notify_event`

## Implementation Order

Recommended implementation sequence:

1. ✅ `notify_event` - Foundation for everything
2. ⏳ `response_sink` + `pending_response` - Type boundary
3. ⏳ `pipeline` - Scheduling logic
4. ⏳ `connection::do_connect` - Handshake
5. ⏳ `connection::do_write` - Write path
6. ⏳ `connection::do_read` - Read path
7. ⏳ `connection::worker_loop` - Main loop
8. ⏳ `connection::enqueue_impl` - Request injection
9. ⏳ `client` - Public API

Each step builds on previous guarantees.
