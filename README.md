# rediscoro

`rediscoro` is a C++20 coroutine Redis client (RESP3) built on top of [`iocoro`](https://github.com/yxzchen/iocoro).

## Status

- `0.x` preview (API may change)
- Linux-focused (inherits `iocoro` backend support)
- Header-only library

## Minimal Example

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

  auto cr = co_await c.connect();
  if (!cr) {
    std::cerr << "connect failed: " << cr.error().to_string() << "\n";
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

## Build

Build tests:

```bash
cmake -S . -B build-test -DREDISCORO_BUILD_TESTS=ON -DREDISCORO_BUILD_EXAMPLES=OFF
cmake --build build-test -j
ctest --test-dir build-test --output-on-failure
```

Build example:

```bash
cmake -S . -B build-example -DREDISCORO_BUILD_TESTS=OFF -DREDISCORO_BUILD_EXAMPLES=ON
cmake --build build-example -j
./build-example/examples/ping
```

## License

MIT, see [`LICENSE`](LICENSE).
