#include <rediscoro/rediscoro.hpp>

#include <iocoro/iocoro.hpp>

#include <cstdint>
#include <iostream>
#include <string>

auto static_response_task() -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;

  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = 6379;
  cfg.reconnection.enabled = false;

  rediscoro::client c{ex, cfg};

  auto cr = co_await c.connect();
  if (!cr) {
    std::cerr << "connect failed: " << cr.error().to_string() << "\n";
    co_return;
  }

  auto set_resp = co_await c.exec<std::string>("SET", "rediscoro:example:counter", "41");
  if (!set_resp.get<0>()) {
    std::cerr << "SET failed: " << set_resp.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  rediscoro::request req{};
  req.push("INCR", "rediscoro:example:counter");
  req.push("GET", "rediscoro:example:counter");

  auto resp = co_await c.exec<std::int64_t, std::string>(std::move(req));
  if (!resp.get<0>()) {
    std::cerr << "INCR failed: " << resp.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }
  if (!resp.get<1>()) {
    std::cerr << "GET failed: " << resp.get<1>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  std::cout << "INCR => " << *resp.get<0>() << "\n";
  std::cout << "GET  => " << *resp.get<1>() << "\n";
  co_await c.close();
}

int main() {
  iocoro::io_context ctx;
  iocoro::co_spawn(ctx.get_executor(), static_response_task(), iocoro::detached);
  ctx.run();
  return 0;
}
