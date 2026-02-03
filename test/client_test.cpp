#include <gtest/gtest.h>

#include <rediscoro/client.hpp>
#include <rediscoro/config.hpp>

#include <iocoro/iocoro.hpp>

#include <chrono>
#include <string>

using namespace std::chrono_literals;

TEST(client_test, connect_to_http_server_reports_protocol_error) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  rediscoro::config cfg{};
  cfg.host = "qq.com";
  cfg.port = 80;
  cfg.resolve_timeout = 1000ms;
  cfg.connect_timeout = 1000ms;
  cfg.reconnection.enabled = false;

  bool skipped = false;
  std::string skip_reason{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    rediscoro::client c{ctx.get_executor(), cfg};

    auto r = co_await c.connect();
    if (r.has_value()) {
      diag = "unexpected success connecting to qq.com:80 as redis";
      co_return;
    }

    auto const e = r.error();

    // If we cannot even resolve/connect, we can't test protocol mismatch deterministically.
    if (e == rediscoro::error::resolve_failed || e == rediscoro::error::resolve_timeout ||
        e == rediscoro::error::connect_failed || e == rediscoro::error::connect_timeout) {
      skipped = true;
      skip_reason = "network not available to reach qq.com:80 (connect failed: " +
                    rediscoro::make_error_code(e).message() + ")";
      co_return;
    }

    // For a reachable HTTP server, RESP3 parser should fail quickly with a protocol error.
    if (static_cast<int>(e) < static_cast<int>(rediscoro::error::resp3_invalid_type_byte)) {
      diag = "expected RESP3 protocol error, got: " + rediscoro::make_error_code(e).message();
      co_return;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  if (skipped) {
    GTEST_SKIP() << skip_reason;
  }
  ASSERT_TRUE(ok) << diag;
}

TEST(client_test, exec_without_connect_is_rejected) {
  iocoro::io_context ctx;

  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    rediscoro::config cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto resp = co_await c.exec<std::string>("PING");
    if (resp.get<0>().has_value()) {
      diag = "expected not_connected error, got value";
      co_return;
    }
    if (!resp.get<0>().error().is_client_error()) {
      diag = "expected client error, got different error category";
      co_return;
    }
    if (resp.get<0>().error().as_client_error() != rediscoro::error::not_connected) {
      diag = "expected not_connected";
      co_return;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_test, resolve_timeout_zero_is_reported) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    rediscoro::config cfg{};
    // Deterministic: resolve_timeout <= 0 makes with_timeout immediately time out.
    cfg.host = "qq.com";
    cfg.port = 80;
    cfg.resolve_timeout = 0ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto r = co_await c.connect();
    if (r.has_value()) {
      diag = "expected resolve_timeout, got success";
      co_return;
    }
    if (r.error() != rediscoro::error::resolve_timeout) {
      diag = "expected resolve_timeout, got: " + rediscoro::make_error_code(r.error()).message();
      co_return;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_test, timeout_error_is_reported_for_unresponsive_peer) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    rediscoro::config cfg{};
    cfg.host = "10.255.255.1";
    cfg.port = 6379;
    cfg.resolve_timeout = 500ms;
    cfg.connect_timeout = 50ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto r = co_await c.connect();
    if (r.has_value()) {
      diag = "unexpected success connecting to blackhole address";
      co_return;
    }

    auto const e = r.error();

    // Depending on routing, we may time out at TCP connect or during the handshake read/write.
    if (!(e == rediscoro::error::connect_timeout || e == rediscoro::error::handshake_timeout ||
          e == rediscoro::error::connect_failed || e == rediscoro::error::resolve_failed ||
          e == rediscoro::error::resolve_timeout || e == rediscoro::error::connection_reset ||
          e == rediscoro::error::operation_aborted)) {
      diag = "expected timeout/connect failure, got: " + rediscoro::make_error_code(e).message();
      co_return;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_test, ping_set_get_roundtrip) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  bool skipped = false;
  std::string skip_reason{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    rediscoro::config cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.resolve_timeout = 500ms;
    cfg.connect_timeout = 500ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto r = co_await c.connect();
    if (!r.has_value()) {
      skipped = true;
      skip_reason = "redis not available at 127.0.0.1:6379 (" +
                    rediscoro::make_error_code(r.error()).message() + ")";
      co_return;
    }

    {
      auto resp = co_await c.exec<std::string>("PING");
      auto& slot = resp.get<0>();
      if (!slot) {
        diag = "PING failed: " + slot.error().to_string();
        co_return;
      }
      if (*slot != "PONG") {
        diag = "expected PONG, got: " + *slot;
        co_return;
      }
    }

    const std::string key = "rediscoro:test:ping_set_get_roundtrip";
    const std::string value = "42";

    {
      auto resp = co_await c.exec<std::int64_t>("DEL", key);
      auto& slot = resp.get<0>();
      if (!slot) {
        diag = "DEL failed: " + slot.error().to_string();
        co_return;
      }
    }

    {
      auto resp = co_await c.exec<std::string>("SET", key, value);
      auto& slot = resp.get<0>();
      if (!slot) {
        diag = "SET failed: " + slot.error().to_string();
        co_return;
      }
      if (*slot != "OK") {
        diag = "expected OK from SET, got: " + *slot;
        co_return;
      }
    }

    {
      auto resp = co_await c.exec<std::string>("GET", key);
      auto& slot = resp.get<0>();
      if (!slot) {
        diag = "GET failed: " + slot.error().to_string();
        co_return;
      }
      if (*slot != value) {
        diag = "expected GET value " + value + ", got: " + *slot;
        co_return;
      }
    }

    co_await c.close();
    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  if (skipped) {
    GTEST_SKIP() << skip_reason;
  }
  ASSERT_TRUE(ok) << diag;
}
