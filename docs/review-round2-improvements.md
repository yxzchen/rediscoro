# Second Review Round: Implementation-Level Refinements

This document summarizes the implementation-level improvements made in the second review round.

## Overview

The second review identified critical implementation details that, if overlooked, would cause subtle bugs:

1. **notify_event race condition** - wait/notify atomicity
2. **FAILED state drain behavior** - IO operations in error state
3. **response_sink multiple delivery** - pipeline logic errors
4. **adapter type constraints** - concept placeholder
5. **Handshake method removal** - use pipeline for all requests
6. **Reconnection semantics** - explicit non-guarantee

All issues have been addressed with structural guarantees or explicit documentation.

---

## 1. notify_event: Race Condition in wait/notify

### Problem Identified

Original design had a critical race window:

```cpp
// WRONG (has race):
awaitable<void> wait() {
  if (count_ == 0) {           // 1. Check count
    awaiting_ = handle;        // 2. Register handle
    // RACE WINDOW HERE!
    // notify() could increment count and see no waiter
    return suspend;            // 3. Suspend
  }
}
```

**Scenario:**
1. Worker calls `wait()`, sees `count_ == 0`
2. Before registering handle, another thread calls `notify()`
3. `notify()` increments count, sees no waiter, returns
4. Worker registers handle and suspends
5. Worker never wakes up (notify already happened)

### Solution

**Documented atomic sequence requirement:**

The following MUST be atomic:
1. Check count_
2. Decide to suspend
3. Register coroutine_handle
4. Actually suspend

**Correct implementation pattern:**
```cpp
// CORRECT (atomic):
awaitable<void> wait() {
  std::lock_guard lock(mutex_);  // Atomic section start
  if (count_.load() > 0) {
    count_.fetch_sub(1);
    return;  // Don't suspend
  }
  awaiting_ = handle;    // Register
  executor_ = current;   // Capture executor
  // Lock released, suspend decision made atomically
}

void notify() {
  std::lock_guard lock(mutex_);  // Atomic section
  count_.fetch_add(1);
  if (awaiting_.has_value()) {
    // Post resume to captured executor
    executor_.post([h = awaiting_] { h.resume(); });
    awaiting_.reset();
  }
}
```

### Guarantee

- Check + register + suspend decision is atomic
- `notify()` either sees count OR sees awaiting coroutine
- No lost signals possible

**File:** `include/rediscoro/detail/notify_event.hpp`

---

## 2. FAILED State: No Drain, Immediate Stop

### Problem Identified

Original design was ambiguous:
- Can `do_read()` / `do_write()` be called in FAILED state?
- Does worker_loop try to drain pending requests after error?
- What if error occurs mid-drain?

**Dangerous scenario:**
```cpp
while (state_ == OPEN) {
  co_await wakeup_.wait();
  while (has_pending()) {
    co_await do_write();  // Error here → state = FAILED
    // But loop continues?
  }
}
```

### Solution

**Explicit FAILED state behavior:**

1. **No IO in FAILED state**
   - `do_read()` / `do_write()` MUST NOT be called
   - Socket is in unknown state, further IO unsafe

2. **Immediate termination**
   - Worker loop exits immediately when state == FAILED
   - No drain attempt in FAILED state

3. **Cleanup in handle_error()**
   - `handle_error()` sets state = FAILED
   - Calls `pipeline.clear_all(error)` immediately
   - All pending requests get error

**State machine rule:**
```
OPEN → (error) → FAILED
  ↓
handle_error() called immediately:
  - state = FAILED
  - pipeline.clear_all(connection_error)
  - socket.close()
  ↓
worker_loop sees FAILED:
  - Exits immediately (no drain)
  - Transitions to CLOSED
```

### Contrast with CLOSING

**CLOSING (graceful shutdown):**
- Socket is healthy
- Attempts to flush pending writes
- Waits for pending reads (with timeout)
- Then transitions to CLOSED

**FAILED (error):**
- Socket is unhealthy
- NO flush, NO wait
- Immediate clear + close
- Then transitions to CLOSED

### Guarantee

- Once FAILED, no further IO operations
- All pending requests failed immediately
- Clean transition to CLOSED

**Files:**
- `include/rediscoro/detail/connection.hpp`
- `include/rediscoro/detail/connection_state.hpp`

---

## 3. response_sink: Prevent Multiple Delivery

### Problem Identified

Pipeline could theoretically call `deliver()` twice on same sink:

```cpp
// Pipeline bug (possible if not careful):
sink->deliver(msg1);
sink->deliver(msg2);  // Logic error!
```

**Consequences:**
- Wrong response delivered to wrong request
- Some pending_response never completes
- Very hard to debug

### Solution

**Two-layer defense:**

#### Layer 1: Pipeline Responsibility (documented)
- NEVER call deliver() more than once on same sink
- Check `is_complete()` before delivery (defensive)
- Remove sink from awaiting queue after delivery

#### Layer 2: Sink Protection (asserted)
```cpp
template <typename T>
void pending_response<T>::deliver(resp3::message msg) {
  // ASSERT on second delivery (development)
  REDISCORO_ASSERT(!result_.has_value() && "deliver() called twice!");

  // Defensive: ignore in release
  if (result_.has_value()) return;

  // Normal delivery...
  result_ = adapt<T>(msg);
  event_.notify();
}
```

### Responsibility Boundary

**Pipeline must:**
- Track which sinks have been delivered
- Never deliver to same sink twice
- This is a pipeline logic error

**Sink must:**
- Assert on second delivery (development)
- Ignore second delivery (release builds, defensive)
- Signal completion via `is_complete()`

### Guarantee

- Multiple delivery is caught during development (assert)
- Defensive handling in production (ignore)
- Clear responsibility assignment

**Files:**
- `include/rediscoro/detail/response_sink.hpp`
- `include/rediscoro/detail/pending_response.hpp`
- `include/rediscoro/detail/impl/pending_response.ipp`

---

## 4. Adapter: Concept Placeholder for Safe Types

### Problem Identified

Current design relies on documentation to prevent user types with side effects:

```cpp
// DANGEROUS (not prevented):
struct MyType {
  MyType(std::string s) {
    log_to_file(s);  // Side effect!
  }
};

adapt<MyType>(msg);  // log_to_file runs in worker_loop!
```

**Risk:**
- Only documented constraint (⚠️)
- Easy to violate unknowingly
- Symptoms: worker_loop slowdown, hard to debug

### Solution

**Concept placeholder for future enforcement:**

```cpp
/// Concept placeholder for safe response types.
///
/// Future enforcement (not currently checked):
/// - Standard library types (std::string, std::vector, etc.)
/// - Trivial types (int, bool, double, etc.)
/// - Aggregates with safe members only
/// - No user-defined constructors with side effects
///
/// To enable static checking in the future, uncomment and use:
///   template <typename T>
///   concept safe_response_type =
///     std::is_trivial_v<T> ||
///     /* is_standard_library_type<T> */ ||
///     /* is_safe_aggregate<T> */;
///
///   template <safe_response_type T>
///   auto adapt(const resp3::message& msg) -> expected<T, error>;
```

### Benefits

1. **Documents intent** clearly
2. **Reserves design space** for future enforcement
3. **Easy to enable** when ready (uncomment + implement concept)
4. **No runtime cost** (compile-time check)

### Current Status

- ⚠️ Documentation-level constraint
- Concept defined but not enforced
- Future work to implement trait detection

**File:** `include/rediscoro/adapter/adapt.hpp`

---

## 5. Handshake: Use Pipeline, Not Special Methods

### Problem Identified

Original design had separate handshake methods:
- `do_auth()`
- `do_select_db()`
- `do_set_client_name()`

**Issues:**
- Code duplication (handshake vs normal requests)
- Different error handling paths
- Complexity in connection setup

### Solution

**Use pipeline for all requests, including handshake:**

```cpp
awaitable<void> connection::do_connect() {
  // 1. TCP connect
  co_await tcp_connect();

  // 2. Transition to OPEN
  state_ = OPEN;

  // 3. Send handshake via pipeline
  auto hello_resp = co_await send_and_await(request{"HELLO", "3"});
  if (!hello_resp) {
    handle_error(hello_resp.error());
    co_return;
  }

  if (!cfg_.password.empty()) {
    auto auth_resp = co_await send_and_await(
      request{"AUTH", cfg_.username, cfg_.password});
    // ... handle error
  }

  if (cfg_.database != 0) {
    auto select_resp = co_await send_and_await(
      request{"SELECT", std::to_string(cfg_.database)});
    // ... handle error
  }

  // ... etc
}
```

### Benefits

1. **Code reuse** - same pipeline for handshake and user requests
2. **Consistency** - same error handling everywhere
3. **Simplicity** - no special-case code paths
4. **Flexibility** - easy to add more handshake commands

### Guarantee

- No special handshake methods
- All requests go through pipeline
- Consistent error handling

**Files:**
- `include/rediscoro/detail/connection.hpp` (removed methods)
- `include/rediscoro/detail/impl/connection.ipp` (updated do_connect)

---

## 6. Reconnection: Explicit Non-Guarantee

### Problem Identified

Users might assume automatic reconnection is supported:
- "Sometimes my requests fail" → seen as bug
- Expectation of request replay
- Confusion about FAILED state behavior

### Solution

**Explicit documentation of current limitations:**

```cpp
/// Reconnection semantics (IMPORTANT - NON-GUARANTEE):
/// Current design does NOT support automatic reconnection or request replay.
///
/// - Once in FAILED state, connection stays FAILED → CLOSED
/// - All pending requests are failed with connection_error
/// - Requests enqueued during FAILED/CLOSED are rejected immediately
/// - NO automatic FAILED → CONNECTING transition
/// - NO request replay across connection failures
///
/// This is an explicit design choice, not an oversight.
/// Automatic reconnection adds significant complexity and is deferred.
```

### Future Considerations (if implemented)

If reconnection is added:
- New `RECONNECTING` state
- Separate `reconnection_policy` component
- Request replay buffer with memory management
- Idempotency handling (not all Redis commands safe to replay)
- Exponential backoff
- Max retry limits

### Current Workaround

Client-level reconnection:
```cpp
auto result = co_await client.execute<std::string>("GET", "key");
if (!result && is_connection_error(result.error())) {
  // Connection failed, create new client
  client = rediscoro::client{ctx.get_executor(), cfg};
  co_await client.connect();

  // Retry safe operations
  result = co_await client.execute<std::string>("GET", "key");
}
```

### Guarantee

- No automatic reconnection (explicit)
- No request replay (explicit)
- Users know what to expect
- Future extension path documented

**Files:**
- `include/rediscoro/detail/connection_state.hpp`
- `docs/architecture.md`

---

## Summary of Improvements

| Issue | Risk Level | Solution | Status |
|-------|-----------|----------|--------|
| notify_event race | 🔴 CRITICAL | Documented atomic sequence requirement | ✅ |
| FAILED drain behavior | 🔴 CRITICAL | Explicit "no IO in FAILED" rule | ✅ |
| Multiple delivery | 🟡 HIGH | Assert + defensive ignore | ✅ |
| Adapter constraints | 🟡 MEDIUM | Concept placeholder | ✅ |
| Handshake methods | 🟢 LOW | Use pipeline for all requests | ✅ |
| Reconnection expectations | 🟢 LOW | Explicit non-guarantee | ✅ |

---

## Implementation Checklist

When implementing these components, verify:

### notify_event
- [ ] Check + register + suspend is atomic (under mutex)
- [ ] notify() sees either count OR awaiting coroutine
- [ ] No race window between check and register
- [ ] Executor captured before suspend

### worker_loop
- [ ] Exit immediately when state == FAILED
- [ ] No do_read/do_write calls in FAILED state
- [ ] handle_error() clears pipeline before setting FAILED

### pending_response
- [ ] Assert fires on second deliver() in debug builds
- [ ] Second deliver() ignored in release builds
- [ ] result_.has_value() checked before setting

### Handshake
- [ ] All handshake commands use pipeline
- [ ] No special-case handshake methods
- [ ] Errors handled consistently

---

## Testing Requirements

### Unit Tests
- [ ] notify_event: concurrent wait/notify stress test
- [ ] connection: FAILED state immediate exit
- [ ] pending_response: double deliver assertion

### Integration Tests
- [ ] Handshake via pipeline
- [ ] Connection failure handling
- [ ] No reconnection on FAILED

### Stress Tests
- [ ] 10000+ concurrent notify() calls
- [ ] Socket errors during drain
- [ ] Double deliver detection

---

## Risk Mitigation

| Original Risk | Mitigation | Confidence |
|---------------|------------|------------|
| Lost notify signals | Atomic check+register+suspend | 🟢 High |
| IO in bad state | Explicit FAILED behavior | 🟢 High |
| Wrong response delivery | Assert + defensive ignore | 🟢 High |
| User type side effects | Documented + concept placeholder | 🟡 Medium |
| Handshake complexity | Use pipeline | 🟢 High |
| Reconnection confusion | Explicit documentation | 🟢 High |

---

## Documentation Updates

All improvements are reflected in:

1. **Header comments** - Implementation contracts
2. **Architecture doc** - High-level design
3. **Review checklists** - Verification criteria
4. **This document** - Implementation rationale

---

## Conclusion

This second review round caught critical implementation details that are easy to get wrong:

- **notify_event race** would cause silent hangs
- **FAILED drain** would attempt IO on bad socket
- **Multiple delivery** would cause wrong responses

All issues now have:
- ✅ Explicit documentation
- ✅ Implementation guidance
- ✅ Defensive assertions
- ✅ Clear responsibility assignment

The design is now ready for implementation with confidence.
