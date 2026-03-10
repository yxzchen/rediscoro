# rediscoro Architecture

## System Picture

```text
+--------------------------------------------------------------+
|                     User Coroutines / App Code               |
|          await client.connect() / exec() / close()          |
+-------------------------------+------------------------------+
                                |
                                v
+--------------------------------------------------------------+
|                        Public API Layer                      |
|  client | request | response<Ts...> | dynamic_response<T>   |
|  config | tracing hooks | error_info | adapter::adapt<T>()  |
+-------------------------------+------------------------------+
                                |
                                v
+--------------------------------------------------------------+
|                     Connection Runtime Layer                 |
|  detail::connection | connection_executor | stop_scope       |
|  actor_loop         | write/read/control loops              |
+-------------------------------+------------------------------+
                                |
                                v
+--------------------------------------------------------------+
|                     Request / Response Core                  |
|  pipeline | pending_response | response_sink | deadlines     |
+-------------------------------+------------------------------+
                                |
                                v
+--------------------------------------------------------------+
|                     RESP3 Data Processing                    |
|  request wire builder | resp3::parser | raw_tree | builder   |
|  resp3::message       | response_builder | adapters          |
+-------------------------------+------------------------------+
                                |
                                v
+--------------------------------------------------------------+
|                       iocoro Runtime / TCP                   |
|  any_io_executor | strand | tcp::socket | resolver          |
|  timers | cancellation | Linux-focused backend              |
+--------------------------------------------------------------+
```

## Repository Layout

```text
/include/rediscoro/          public headers and user-facing API
/include/rediscoro/detail/   internal connection and pipeline primitives
/include/rediscoro/impl/     inline .ipp implementations
/include/rediscoro/resp3/    RESP3 parser, raw tree, builder, message model
/include/rediscoro/adapter/  RESP3-to-C++ adaptation layer
/examples/                   runnable usage samples
/test/                       unit and integration-style tests
/benchmark/                  performance suites and report tooling
```

Important packaging note:

```text
CMake target: rediscoro
Type:         INTERFACE library
Meaning:      header-only Redis client built on top of iocoro
```

## Runtime Center

```text
client
   |
   +-- shared_ptr<detail::connection>
           |
           +-- connection_executor
           |      |
           |      +-- io executor
           |      +-- strand executor
           |
           +-- tcp::socket
           +-- pipeline
           +-- resp3::parser
           +-- stop_scope
           +-- write_wakeup / read_wakeup / control_wakeup
           |
           +-- actor_loop()
                  |
                  +-- write_loop()
                  +-- read_loop()
                  +-- control_loop()
```

`detail::connection` is the runtime center of the library.
It owns the state machine that drives:

- connection lifecycle
- request enqueue and backpressure
- socket read/write progress
- runtime error handling and reconnection
- request timeout enforcement

## Connection Lifecycle

```text
INIT -> CONNECTING -> OPEN
  |        |           |
  |        |           +--> FAILED -> RECONNECTING -> OPEN
  |        |                        |
  |        +------------------------+
  |                                 |
  +-----------------------------> CLOSING -> CLOSED
```

Key semantics:

- only `OPEN` accepts user requests
- initial `connect()` failures do not enter `FAILED`
- `FAILED` is reserved for runtime failures after reaching `OPEN`
- `CLOSED` is the terminal state for one actor lifetime, but a later `connect()` may reset it
- only `transition_to_closed()` writes `CLOSED`

## Connect And Handshake Path

```text
connect()
   |
   +--> switch to connection strand
   +--> run_actor() if needed
   +--> state = CONNECTING
   +--> do_connect()
           |
           +--> resolver.async_resolve(host, port)
           +--> socket.async_connect(endpoint)
           +--> set TCP options
           +--> build handshake request:
                   HELLO 3
                   AUTH
                   SELECT
                   CLIENT SETNAME
           +--> drive handshake write/read directly
           +--> validate replies
           +--> state = OPEN
```

Notable design choice:

- handshake is encoded as a normal `request` and completed through the same `pipeline`
- while `state != OPEN`, normal runtime read/write loops are gated and do not touch the socket

## Actor Execution Model

```text
actor_loop()
   |
   +--> co_spawn(write_loop)
   +--> co_spawn(read_loop)
   +--> co_spawn(control_loop)
   |
   +--> when_all(...)
   |
   +--> transition_to_closed()
```

Role of each loop:

- `write_loop`
  flush pending request wire bytes when `state == OPEN`
- `read_loop`
  perform socket reads and parse RESP3 messages when `state == OPEN`
- `control_loop`
  own reconnection policy and request-timeout waiting

Important invariant:

```text
One connection
    -> one strand-serialized internal state machine
    -> at most one in-flight async_read_some
    -> at most one in-flight async_write_some
```

## Request Execution Path

```text
client.exec(...)
   |
   +--> connection.enqueue<T...>()
           |
           +--> strand.dispatch(enqueue_impl)
                   |
                   +--> state gating
                   +--> trace start (optional)
                   +--> pipeline.push(request, sink, deadline)
                   +--> notify write_loop / control_loop
```

Normal completion path:

```text
pipeline pending_write_
        |
        +--> write_loop -> do_write()
                |
                +--> socket.async_write_some(...)
                +--> pipeline.on_write_done()
                +--> move sink to awaiting_read_
        |
        +--> read_loop -> do_read()
                |
                +--> socket.async_read_some(...)
                +--> parser.parse_one()
                +--> resp3::build_message(...)
                +--> pipeline.on_message(msg)
                +--> sink.deliver(...)
                +--> waiting coroutine resumes
```

This preserves Redis pipeline ordering:

- requests are written FIFO
- replies are delivered FIFO
- a request with multiple commands consumes multiple reply slots on the same sink

## Pipeline Scheduler

```text
pipeline
   |
   +-- pending_write_   -> requests not fully written yet
   +-- awaiting_read_   -> sinks waiting for replies
   +-- pending_write_bytes_
   +-- max_requests / max_pending_write_bytes
```

`pipeline` is intentionally narrow in responsibility.
It knows:

- request wire bytes
- FIFO scheduling
- response delivery order
- queue limits and deadlines

It does not know:

- socket operations
- executors or coroutine handles
- reconnect policy
- concrete response types beyond `response_sink`

## RESP3 Processing Path

```text
socket bytes
   |
   +--> resp3::buffer
   +--> resp3::parser
           |
           +--> raw_tree + root index
           |
           +--> resp3::build_message(...)
                   |
                   +--> resp3::message
                           |
                           +--> response_builder / adapter::adapt<T>()
```

Layer roles:

- `request`
  builds RESP3 array-of-bulk-strings wire bytes for commands
- `resp3::parser`
  incremental zero-copy parser that produces a raw tree
- `resp3::build_message`
  converts the raw tree into an owning `resp3::message`
- `response_builder`
  maps RESP3 replies into typed response slots
- `adapter`
  converts `resp3::message` into user-visible C++ types

Important lifetime rule:

```text
parse_one() success
    -> consume tree/root
    -> build message
    -> reclaim parser state
```

The raw tree holds `string_view`s into parser-managed buffer memory, so `reclaim()` is part of the
message boundary contract, not a cosmetic cleanup step.

## Response Model

```text
response<Ts...>
   |
   +-- tuple<expected<T, error_info>...>

dynamic_response<T>
   |
   +-- vector<expected<T, error_info>>
```

Main idea:

- failures are per-slot, not all-or-nothing for the whole pipeline
- Redis error replies become `server_errc::redis_error`
- protocol / connection / timeout failures become `error_info`
- type adaptation failures become `adapter_errc::*` wrapped in `error_info`

This lets one pipelined request contain mixed outcomes:

- successful values
- Redis command errors
- local adaptation failures
- connection-level failures fanned out to all remaining slots

## Adapter Model

```text
adapter::adapt<T>()
   |
   +-- scalar targets
   +-- optional<T>
   +-- std::array<T, N>
   +-- sequence-like containers
   +-- map-like containers
   +-- ignore_t
```

Behavior summary:

- scalar adapters require compatible RESP3 kinds and perform numeric range checks
- `optional<T>` maps RESP3 null to `nullopt`
- sequences consume RESP3 array/set/push aggregates
- maps consume RESP3 map and detect duplicate keys
- `ignore_t` always succeeds at the top level

Important constraint:

- adaptation happens on the connection strand during response delivery
- target types should be passive and non-blocking to construct or append into
- `std::string_view` is intentionally rejected as an output type to avoid dangling views

## Blocking Work Path

```text
connect()
   |
   +--> resolver.async_resolve(...)
           |
           +--> iocoro background thread_pool
                   |
                   +--> blocking getaddrinfo()
                   |
                   +--> resume on connection strand
```

Why this exists:

- TCP readiness lives on the iocoro IO runtime
- DNS resolution is still blocking work underneath
- the library keeps that blocking step outside the socket actor loop

## Timeout, Error, And Reconnection Path

```text
request timeout / read error / write error / parse error
        |
        v
handle_error(error_info)
        |
        +--> OPEN -> FAILED
        +--> pipeline.clear_all(...)
        +--> close socket
        +--> wake control_loop
                    |
                    +--> reconnect disabled -> CLOSING
                    +--> reconnect enabled  -> do_reconnect()
```

Current timeout policy:

- each accepted request gets one deadline
- `control_loop()` watches the earliest pending deadline
- when a request deadline expires, the library treats it as a connection-level failure

This is intentionally conservative:

- timeout does not fail just one request and keep the connection alive
- timeout tears down the current generation and may trigger reconnect

## Shutdown And Cancellation Path

```text
close() / stop request
        |
        +--> state = CLOSING
        +--> stop_scope.request_stop()
        +--> pipeline.clear_all(connection_closed)
        +--> socket.close()
        +--> wake all loops
        +--> wait actor_done_
        +--> transition_to_closed()
```

Shutdown in this project is:

- deterministic-first
- connection-wide
- cooperative rather than preemptive

One important caveat remains:

- resolver cancellation is best-effort only because in-flight `getaddrinfo()` cannot be forcibly
  interrupted by the current model

## Concurrency Model

```text
Foreign threads / foreign executors
    |
    +-- client.exec(...)
    +-- client.connect()
    +-- client.close()
    |
    v
connection strand dispatch/switch
    |
    v
serialized mutation of:
    - state_
    - pipeline_
    - socket lifecycle
    - parser lifecycle
```

The strategy is intentionally conservative:

- external callers may invoke API from any executor
- actual internal mutation always happens on one strand
- there is no request buffering across connection generations
- full-duplex socket use is allowed, but only one read and one write may be in flight

## Main Design Choices

```text
rediscoro
  |
  +-- header-only packaging
  +-- C++20 coroutine-based API
  +-- typed per-slot pipeline responses
  +-- strand-serialized connection actor
  +-- normal-request handshake model
  +-- zero-copy RESP3 parse then owning message build
  +-- adapter-based conversion into user C++ types
  +-- deterministic close and conservative reconnect policy
```

What stands out architecturally:

- the public client stays very small while the runtime complexity is concentrated in one connection
  actor
- the request path, handshake path, and reconnect path reuse the same pipeline model
- parsing, message materialization, and type adaptation are clearly separated
- response delivery is intentionally abstracted behind `response_sink` so the pipeline never
  resumes user code inline

## Current Architectural Boundaries

```text
Scope today
  |
  +-- Linux-focused through iocoro
  +-- header-only library
  +-- one strand-serialized runtime per connection
  +-- no request buffering before OPEN or across reconnect generations
  +-- unsolicited RESP3 PUSH messages treated as an error path
  +-- request timeout handled as a connection-level failure
  +-- cooperative cancellation with best-effort DNS interruption
```

These are current design boundaries, not accidental omissions.
