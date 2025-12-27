# rediscoro

A modern C++20 Redis client built on coroutines and the [ioxz](https://github.com/yxzchen/ioxz) async I/O library.

This project is under development. It has not been fully tested and its API is not yet considered stable.

## Features

- **C++20 coroutines** with `co_await` async API
- **RESP3 protocol** support (Redis 6+)
- **Type-safe responses** with automatic conversion
- **Pipelined requests** for efficient batching
- **Auto-reconnect** on connection failure

## Example Usage

```cpp
#include <rediscoro/src.hpp>  // Include once in one .cpp file
#include <rediscoro.hpp>
#include <xz/io/io_context.hpp>
#include <iostream>

using namespace rediscoro;
using namespace xz::io;

awaitable<void> example() {
  // Configure and connect
  config cfg;
  cfg.host = "127.0.0.1";
  cfg.port = 6379;
  
  connection conn{co_await this_coro::executor, cfg};
  co_await conn.run();
  
  // Single command
  request req;
  req.push("SET", "key", "value");
  auto resp = co_await conn.execute_one<std::string>(req);
  
  if (resp.has_value()) {
    std::cout << "SET result: " << resp.value() << "\n";
  }
  
  // Pipeline multiple commands
  request multi;
  multi.push("INCR", "counter");
  multi.push("GET", "key");
  
  auto [count, value] = co_await conn.execute_one<int, std::string>(multi);
  
  if (count.has_value() && value.has_value()) {
    std::cout << "Counter: " << count.value() << "\n";
    std::cout << "Value: " << value.value() << "\n";
  }
  
  co_await conn.graceful_stop();
}

int main() {
  io_context ctx;
  co_spawn(ctx, example(), use_detached);
  ctx.run();
}
```

## Dependencies

- C++20 compiler (GCC 10+, Clang 12+)
- [ioxz](https://github.com/yxzchen/ioxz) - io_uring/epoll async I/O library
- [GoogleTest](https://github.com/google/googletest) (tests only)

## Integration

### CMake

```cmake
add_subdirectory(rediscoro)
target_link_libraries(your_app PRIVATE rediscoro)
```

### Manual

1. Add `rediscoro/include` to your include path
2. In your headers: `#include <rediscoro.hpp>`
3. In exactly **one** source file: `#include <rediscoro/src.hpp>`

## Supported Features

- RESP3 and RESP2 protocols  
- Automatic handshake (HELLO, AUTH, SELECT, CLIENT SETNAME)  
- Type-safe response parsing (int, string, vector, map, set, optional, etc.)  
- Request pipelining with multiple commands  
- Auto-reconnect with configurable delay  
- Connection timeouts (connect, request)  
- Graceful shutdown  
- Per-reply error handling  
- FIFO request queue with optional max inflight limit  

## Planned Features

- Pub/Sub (push message handling)  
- Transactions (MULTI/EXEC)  
- Health check (idle PING)  
- Connection pooling  
- Redis Cluster support  
- Lua script execution  
- Streams support  
- TLS/SSL  
- Command builder DSL  
