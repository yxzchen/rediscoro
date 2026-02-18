#include <rediscoro/rediscoro.hpp>

#include <iocoro/iocoro.hpp>

#include <chrono>
#include <iostream>

using namespace std::chrono_literals;

auto request_timeout_task() -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;

  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = 6379;
  cfg.reconnection.enabled = false;
  cfg.request_timeout = 100ms;

  rediscoro::client c{ex, cfg};

  auto cr = co_await c.connect();
  if (!cr) {
    std::cerr << "connect failed: " << cr.error().to_string() << "\n";
    co_return;
  }

  // BLPOP on a missing key blocks on Redis side. With a short request_timeout, this should fail
  // locally with client_errc::request_timeout before Redis returns.
  auto resp = co_await c.exec<rediscoro::ignore_t>("BLPOP", "rediscoro:example:missing-list", "5");
  if (resp.get<0>()) {
    std::cout << "BLPOP returned before timeout (unexpected for this demo)\n";
  } else {
    auto const& err = resp.get<0>().error();
    std::cout << "BLPOP failed: " << err.to_string() << "\n";
    if (err.code == make_error_code(rediscoro::client_errc::request_timeout)) {
      std::cout << "Observed expected error: client_errc::request_timeout\n";
    }
  }

  co_await c.close();
}

int main() {
  iocoro::io_context ctx;
  iocoro::co_spawn(ctx.get_executor(), request_timeout_task(), iocoro::detached);
  ctx.run();
  return 0;
}
