#include <gtest/gtest.h>

#include <rediscoro/client.hpp>
#include <rediscoro/config.hpp>

#include <iocoro/iocoro.hpp>

#include <chrono>
#include <string>

using namespace std::chrono_literals;

TEST(redis_integration, ping_set_get) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = 6379;
  cfg.connect_timeout = 300ms;
  cfg.request_timeout = 500ms;
  cfg.reconnection.enabled = false;  // integration test should fail fast when Redis is absent

  bool skipped = false;
  std::string skip_reason{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    rediscoro::client c{ctx.get_executor(), cfg};

    auto ec = co_await c.connect();
    if (ec) {
      skipped = true;
      skip_reason = "Redis not reachable at 127.0.0.1:6379 (connect failed: " + ec.message() + ")";
      guard.reset();
      co_return;
    }

    auto pong = co_await c.exec<std::string>("PING");
    if (!pong.get<0>().has_value()) {
      diag = "PING failed";
      guard.reset();
      co_return;
    }
    if (*pong.get<0>() != "PONG") {
      diag = "PING unexpected reply: " + *pong.get<0>();
      guard.reset();
      co_return;
    }

    auto setr = co_await c.exec<std::string>("SET", "rediscoro_test_key", "v1");
    if (!setr.get<0>().has_value()) {
      diag = "SET failed";
      guard.reset();
      co_return;
    }
    if (*setr.get<0>() != "OK") {
      diag = "SET unexpected reply: " + *setr.get<0>();
      guard.reset();
      co_return;
    }

    auto getr = co_await c.exec<std::string>("GET", "rediscoro_test_key");
    if (!getr.get<0>().has_value()) {
      diag = "GET failed";
      guard.reset();
      co_return;
    }
    if (*getr.get<0>() != "v1") {
      diag = "GET unexpected reply: " + *getr.get<0>();
      guard.reset();
      co_return;
    }

    ok = true;

    co_await c.close();
    guard.reset();
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  if (skipped) {
    GTEST_SKIP() << skip_reason;
  }
  ASSERT_TRUE(ok) << diag;
}


