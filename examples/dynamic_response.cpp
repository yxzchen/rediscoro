#include <rediscoro/rediscoro.hpp>

#include <iocoro/iocoro.hpp>

#include <cstdint>
#include <iostream>
#include <string>

auto dynamic_response_task() -> iocoro::awaitable<void> {
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

  auto set_a = co_await c.exec<std::string>("SET", "rediscoro:example:counter:a", "0");
  if (!set_a.get<0>()) {
    std::cerr << "SET counter a failed: " << set_a.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  auto set_b = co_await c.exec<std::string>("SET", "rediscoro:example:counter:b", "100");
  if (!set_b.get<0>()) {
    std::cerr << "SET counter b failed: " << set_b.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  rediscoro::request req{};
  req.push("INCR", "rediscoro:example:counter:a");
  req.push("INCR", "rediscoro:example:counter:b");
  req.push("INCR", "rediscoro:example:counter:a");

  auto resp = co_await c.exec_dynamic<std::int64_t>(std::move(req));
  for (std::size_t i = 0; i < resp.size(); ++i) {
    if (!resp[i]) {
      std::cerr << "INCR #" << i << " failed: " << resp[i].error().to_string() << "\n";
      co_await c.close();
      co_return;
    }
    std::cout << "INCR #" << i << " => " << *resp[i] << "\n";
  }

  co_await c.close();
}

int main() {
  iocoro::io_context ctx;
  iocoro::co_spawn(ctx.get_executor(), dynamic_response_task(), iocoro::detached);
  ctx.run();
  return 0;
}
