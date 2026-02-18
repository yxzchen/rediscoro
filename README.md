# rediscoro

`rediscoro` is a **0.x preview** coroutine-based Redis client for C++20, built on top of
[`iocoro`](https://github.com/yxzchen/iocoro).

It focuses on a clear async API, a strand-serialized connection actor, and an incremental RESP3
parser.

## Status & stability

- **Preview/testing release (0.x)**: APIs and behavior may change.
- **OS**: Linux (inherited from `iocoro` backends).
- **Protocol**: RESP3 handshake (`HELLO 3`) is used.

## API boundary

- **Public headers**: `include/rediscoro/*.hpp` (for example `<rediscoro/rediscoro.hpp>` and
  `<rediscoro/client.hpp>`). These are the supported user-facing entry points.
- **Internal headers**: `include/rediscoro/detail/*.hpp`. These are implementation details and are
  not a stable API contract.
- `rediscoro` is header-only, so internal headers are installed with the package for build
  completeness, but they may change without compatibility guarantees.

## Key capabilities

- **Coroutine-friendly API**: `connect()`, `exec<T>()`, `close()`.
- **Pipelining**: one `request` can contain multiple commands; replies are delivered in-order.
- **Typed responses**: `response<Ts...>` / `dynamic_response<T>` with `expected<T, error_info>` slots.
- **Timeouts**: resolve/connect/request timeout policies (see `rediscoro::config`).
- **Auto reconnect**: configurable backoff policy (see `reconnection_policy`).
- **Tracing hooks**: lightweight request start/finish callbacks (see `request_trace_hooks`).

## Requirements

- CMake >= 3.15
- A C++20 compiler (GCC / Clang with coroutine support)
- `iocoro` (via `find_package(iocoro)` or fetched automatically when building `rediscoro`)

## Minimal example

```cpp
#include <rediscoro/rediscoro.hpp>

#include <iocoro/iocoro.hpp>

#include <iostream>
#include <string>

auto ping_task() -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;

  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = 6379;

  rediscoro::client c{ex, cfg};

  auto r = co_await c.connect();
  if (!r) {
    std::cerr << "connect failed: " << r.error().to_string() << "\n";
    co_return;
  }

  auto resp = co_await c.exec<std::string>("PING");
  if (!resp.get<0>()) {
    std::cerr << "PING failed: " << resp.get<0>().error().to_string() << "\n";
    co_return;
  }

  std::cout << "PING => " << *resp.get<0>() << "\n";
  co_await c.close();
}

int main() {
  iocoro::io_context ctx;
  iocoro::co_spawn(ctx.get_executor(), ping_task(), iocoro::detached);
  ctx.run();
  return 0;
}
```

See [`examples/`](examples/) for a buildable example.

## Build & test

```bash
./scripts/build.sh -t
```

Sanitizers (Debug):

```bash
./scripts/build.sh -t --asan
./scripts/build.sh -t --ubsan
./scripts/build.sh -t --tsan
```

## Install and use with CMake (`find_package`)

Install to a prefix:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build -j
cmake --install build
```

Consume:

```cmake
find_package(rediscoro REQUIRED)
target_link_libraries(your_target PRIVATE rediscoro::rediscoro)
```

## License

MIT License. See [`LICENSE`](LICENSE).
