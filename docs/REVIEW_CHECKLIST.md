# Code Review Checklist

Use this checklist when reviewing connection-related code or implementing new features.

## Critical Invariants (MUST NOT VIOLATE)

### ✅ Socket Single Ownership
- [ ] worker_loop is the ONLY coroutine accessing socket
- [ ] No `co_spawn` calls within connection internals
- [ ] do_read/do_write are subroutines, not spawned coroutines

**Why:** Violating this causes data races on socket state.

---

### ✅ Executor-Correct Resumption
- [ ] notify_event captures executor at wait() time
- [ ] notify() posts to captured executor (never inline resume)
- [ ] No direct coroutine_handle.resume() calls

**Why:** User code must run on user's executor, not connection strand.

---

### ✅ No User Code in Worker Loop
- [ ] Pipeline operates only on response_sink* (abstract interface)
- [ ] pending_response::deliver() only stores and notifies
- [ ] adapter::adapt<T> uses only standard/trivial types
- [ ] No user callbacks, logging, or side effects in deliver path

**Why:** User code in worker loop can block socket operations.

---

### ✅ Type Isolation
- [ ] Pipeline cannot access pending_response<T> directly
- [ ] Pipeline cannot access coroutine_handle
- [ ] All pipeline operations use response_sink* only

**Why:** Type system prevents accidental inline resumption.

---

### ✅ Actor Model (No Shared Mutable State)
- [ ] All state mutations happen on connection strand
- [ ] No mutex/locks (except notify_event internals)
- [ ] enqueue() posts to strand, doesn't touch state directly

**Why:** Locks everywhere = deadlock potential.

---

### ✅ Counting Notification (No Signal Loss)
- [ ] notify_event uses atomic counter
- [ ] Worker loop drains ALL work before sleeping
- [ ] Never suspend while work remains

**Why:** Lost signals = deadlocked requests.

---

## State Machine Compliance

### State Transition Rules
- [ ] INIT → CONNECTING only (via start())
- [ ] CONNECTING → OPEN or FAILED only
- [ ] OPEN → CLOSING (via stop()) or FAILED (via error)
- [ ] FAILED → RECONNECTING (immediate) OR FAILED (sleep/backoff) OR CLOSED (if reconnection disabled / cancelled)
- [ ] RECONNECTING → OPEN (success) OR FAILED (failure)
- [ ] CLOSING → CLOSED only
- [ ] CLOSED is terminal (no transitions out)

### Operation Semantics
- [ ] enqueue() in INIT/CONNECTING/OPEN: ACCEPT
- [ ] enqueue() in FAILED (backoff window): REJECT with connection_error
- [ ] enqueue() in RECONNECTING: ACCEPT (queue for after reconnect)
- [ ] enqueue() in CLOSING/CLOSED: REJECT with connection_closed
- [ ] stop() in INIT: immediate CLOSED
- [ ] stop() in CONNECTING/OPEN: transition to CLOSING
- [ ] stop() in FAILED/RECONNECTING/CLOSING/CLOSED: request cancel + wait for CLOSED

---

## Completion Semantics

### do_write()
- [ ] Attempts to write ALL pending requests
- [ ] Returns when: all written OR socket blocks OR error
- [ ] Partial writes tracked via pipeline.on_write_done()

### do_read()
- [ ] Attempts to read at least ONE complete RESP3 message
- [ ] Returns when: message parsed + socket blocks OR no pending reads OR error
- [ ] NEVER returns without reading or encountering error/block

### worker_loop Drain Strategy
- [ ] After wakeup, loop until no work remains
- [ ] Check: has_pending_write() || has_pending_read()
- [ ] Only suspend when work queue is empty
- [ ] Once state becomes FAILED, do NOT perform any further do_read/do_write (only cleanup + reconnection path)

---

## Thread Safety

### Strand-Only Operations (Majority)
- [ ] worker_loop internals
- [ ] pipeline methods
- [ ] response_sink::deliver()
- [ ] Socket operations
- [ ] State transitions
- [ ] enqueue_impl()

### Cross-Executor Safe (Rare)
- [ ] connection::enqueue() - posts to strand
- [ ] notify_event::notify() - atomic + post
- [ ] cancel_source::request_cancel() - atomic store

---

## Executor Constraints (Construction)

### ✅ IO Executor Required
- [ ] `client` is constructed with `iocoro::io_executor`
- [ ] `connection` is constructed with `iocoro::io_executor`

**Why:** Socket IO requires an event loop; other executors cannot drive network operations.

---

## Forbidden Patterns

### ❌ NEVER Do This

```cpp
// WRONG: Nested co_spawn
co_spawn(executor_.get(), async_helper(), detached);

// WRONG: Bypass strand
co_await socket_.async_read_some(buffer, use_awaitable);

// WRONG: Direct resume
coroutine_handle.resume();

// WRONG: User type with side effects
struct BadType {
  BadType(std::string s) { log_to_file(s); }
};
adapt<BadType>(msg);  // BAD: log runs in worker loop

// WRONG: Mutex in connection
std::lock_guard lock(mutex_);  // Should never need this!

// WRONG: Blocking in deliver()
void deliver(msg) override {
  heavy_computation();  // BLOCKS WORKER LOOP!
  event_.notify();
}
```

### ✅ Correct Patterns

```cpp
// OK: Subroutine call
co_await do_read();

// OK: Always use strand executor
auto ex = executor_.get();

// OK: notify_event handles resume
event_.notify();

// OK: Standard types only
adapt<std::string>(msg);
adapt<int64_t>(msg);
adapt<std::vector<int>>(msg);

// OK: Actor model (no locks)
void enqueue_impl(req, sink) {
  pipeline_.push(req, sink);  // Single-threaded
  wakeup_.notify();            // Cross-thread safe
}

// OK: Store and notify only
void deliver(msg) override {
  result_ = adapt<T>(msg);
  event_.notify();
}
```

---

## Code Review Questions

Before merging connection-related code, answer:

1. **Can multiple coroutines access the socket?**
   - If yes → REJECT

2. **Does any code path resume a coroutine inline?**
   - If yes → REJECT

3. **Does pipeline know about pending_response<T> or coroutine_handle?**
   - If yes → REJECT

4. **Can user code run on the connection strand?**
   - If yes → REJECT (unless documented and unavoidable)

5. **Are there any locks besides notify_event internals?**
   - If yes → WHY? (probably wrong)

6. **Can signals be lost?**
   - If yes → REJECT (must use counting notification)

7. **Is state machine behavior deterministic?**
   - Must be YES for all states

8. **Are completion semantics documented?**
   - Must be YES for do_read, do_write, worker_loop

---

## Testing Checklist

### Unit Tests
- [ ] notify_event: multiple notify() = multiple wakeups
- [ ] pending_response: deliver() + wait() on different executors
- [ ] pipeline: FIFO ordering preserved
- [ ] State machine: all transitions exercised
- [ ] enqueue(): behavior in all states

### Integration Tests
- [ ] Pipeline: multiple requests in-flight
- [ ] Cancellation: stop() during CONNECTING
- [ ] Error handling: FAILED → CLOSED transition
- [ ] Graceful shutdown: CLOSING flushes pending writes

### Stress Tests
- [ ] 1000+ concurrent requests
- [ ] Rapid enqueue() from multiple threads
- [ ] Socket errors during active requests
- [ ] Worker loop doesn't hang when work available

---

## Documentation Requirements

When adding new features, document:

1. **State machine impact**
   - Which states are affected?
   - New transitions?
   - Per-state operation semantics

2. **Thread safety**
   - Strand-only or cross-executor?
   - New locks needed? (Should be NO!)

3. **Completion semantics**
   - When does the operation return?
   - What guarantees are provided?

4. **Invariant preservation**
   - How are the 6 critical invariants maintained?

---

## Performance Considerations

### Avoid
- ❌ Allocations in hot path (do_read/do_write)
- ❌ Virtual calls in tight loops
- ❌ Mutex contention
- ❌ Unnecessary executor posts

### Prefer
- ✅ Lock-free atomics (notify_event counter)
- ✅ Single-threaded logic (connection strand)
- ✅ Batch operations (drain loop)
- ✅ Zero-copy where possible

---

## When in Doubt

**Ask:**
1. Does this break any of the 6 critical invariants?
2. Does this add locks or shared mutable state?
3. Does this allow user code in worker loop?
4. Does this bypass the strand?
5. Can this cause signal loss?

**If ANY answer is YES → redesign.**

---

## Review Approval Criteria

Code MUST:
- [ ] Pass all invariant checks
- [ ] Have documented state behavior
- [ ] Have documented completion semantics
- [ ] Pass syntax check (g++ -fsyntax-only)
- [ ] Have no TODOs that break invariants

Code SHOULD:
- [ ] Have unit tests
- [ ] Have integration tests
- [ ] Be reviewed by another developer
- [ ] Have updated architecture.md if needed
