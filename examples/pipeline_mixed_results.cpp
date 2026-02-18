#include <rediscoro/rediscoro.hpp>

#include <iocoro/iocoro.hpp>

#include <cstdint>
#include <iostream>
#include <string>

auto pipeline_mixed_results_task() -> iocoro::awaitable<void> {
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

  constexpr auto k_counter_key = "rediscoro:example:mixed:counter";
  auto seed = co_await c.exec<std::string>("SET", k_counter_key, "not-an-integer");
  if (!seed.get<0>()) {
    std::cerr << "SET failed: " << seed.get<0>().error().to_string() << "\n";
    co_await c.close();
    co_return;
  }

  rediscoro::request req{};
  req.push("GET", k_counter_key);   // success
  req.push("INCR", k_counter_key);  // server error

  auto resp = co_await c.exec<std::string, std::int64_t>(std::move(req));

  if (resp.get<0>()) {
    std::cout << "GET  succeeded: " << *resp.get<0>() << "\n";
  } else {
    std::cout << "GET  failed: " << resp.get<0>().error().to_string() << "\n";
  }

  if (resp.get<1>()) {
    std::cout << "INCR succeeded unexpectedly: " << *resp.get<1>() << "\n";
  } else {
    std::cout << "INCR failed as expected: " << resp.get<1>().error().to_string() << "\n";
  }

  co_await c.close();
}

int main() {
  iocoro::io_context ctx;
  iocoro::co_spawn(ctx.get_executor(), pipeline_mixed_results_task(), iocoro::detached);
  ctx.run();
  return 0;
}
