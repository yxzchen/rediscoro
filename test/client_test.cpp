#include <gtest/gtest.h>

#include <rediscoro/client.hpp>
#include <rediscoro/config.hpp>

#include <iocoro/iocoro.hpp>

#include <chrono>
#include <string>

using namespace std::chrono_literals;

TEST(client_test, connect_to_http_server_reports_protocol_error) {
  iocoro::io_context ctx;

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
    rediscoro::client c{ctx.get_executor(), cfg};

    auto ec = co_await c.connect();
    if (!ec) {
      diag = "unexpected success connecting to qq.com:80 as redis";
      co_return;
    }

    // If we cannot even resolve/connect, we can't test protocol mismatch deterministically.
    if (ec == rediscoro::error::resolve_failed || ec == rediscoro::error::resolve_timeout ||
        ec == rediscoro::error::connect_failed || ec == rediscoro::error::connect_timeout) {
      skipped = true;
      skip_reason = "network not available to reach qq.com:80 (connect failed: " + ec.message() + ")";
      co_return;
    }

    // For a reachable HTTP server, RESP3 parser should fail quickly with a protocol error.
    // After unification, RESP3 errors are in rediscoro category (100-199 range).
    if (std::string_view{ec.category().name()} != "rediscoro") {
      diag = "expected rediscoro error category, got: " + std::string{ec.category().name()} +
             " / " + ec.message();
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

  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    rediscoro::config cfg{};
    // Deterministic: resolve_timeout <= 0 makes with_timeout_detached immediately time out.
    cfg.host = "qq.com";
    cfg.port = 80;
    cfg.resolve_timeout = 0ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto ec = co_await c.connect();
    if (ec != rediscoro::error::resolve_timeout) {
      diag = "expected resolve_timeout, got: " + ec.message();
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

  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    rediscoro::config cfg{};
    cfg.host = "10.255.255.1";
    cfg.port = 6379;
    cfg.resolve_timeout = 500ms;
    cfg.connect_timeout = 50ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto ec = co_await c.connect();
    if (!ec) {
      diag = "unexpected success connecting to blackhole address";
      co_return;
    }

    // Depending on routing, we may time out at TCP connect or during the handshake read/write.
    if (!(ec == rediscoro::error::connect_timeout || ec == rediscoro::error::handshake_timeout ||
          ec == rediscoro::error::connect_failed || ec == rediscoro::error::resolve_failed ||
          ec == rediscoro::error::resolve_timeout || ec == rediscoro::error::connection_reset)) {
      diag = "expected timeout/connect failure, got: " + ec.message();
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

  bool skipped = false;
  std::string skip_reason{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    rediscoro::config cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.resolve_timeout = 500ms;
    cfg.connect_timeout = 500ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto ec = co_await c.connect();
    if (ec) {
      skipped = true;
      skip_reason = "redis not available at 127.0.0.1:6379 (" + ec.message() + ")";
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
