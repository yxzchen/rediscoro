#include <gtest/gtest.h>

#include <rediscoro/client.hpp>
#include <rediscoro/config.hpp>

#include <iocoro/iocoro.hpp>

#include <chrono>
#include <string>

using namespace std::chrono_literals;

TEST(client_external, connect_to_http_server_reports_protocol_error) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  rediscoro::config cfg{};
  cfg.host = "apple.com";
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
      diag = "unexpected success connecting to apple.com:80 as redis";
      guard.reset();
      co_return;
    }

    // If we cannot even resolve/connect, we can't test protocol mismatch deterministically.
    if (ec == rediscoro::error::resolve_failed || ec == rediscoro::error::resolve_timeout ||
        ec == rediscoro::error::connect_failed || ec == rediscoro::error::connect_timeout) {
      skipped = true;
      skip_reason = "network not available to reach apple.com:80 (connect failed: " + ec.message() + ")";
      guard.reset();
      co_return;
    }

    // For a reachable HTTP server, RESP3 parser should fail quickly with a protocol error.
    // After unification, RESP3 errors are in rediscoro category (100-199 range).
    if (std::string_view{ec.category().name()} != "rediscoro") {
      diag = "expected rediscoro error category, got: " + std::string{ec.category().name()} +
             " / " + ec.message();
      guard.reset();
      co_return;
    }
    ok = true;
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

TEST(client_external, exec_without_connect_is_rejected) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

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
      guard.reset();
      co_return;
    }
    if (!resp.get<0>().error().is_client_error()) {
      diag = "expected client error, got different error category";
      guard.reset();
      co_return;
    }
    if (resp.get<0>().error().as_client_error() != rediscoro::error::not_connected) {
      diag = "expected not_connected";
      guard.reset();
      co_return;
    }

    ok = true;
    guard.reset();
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_external, resolve_timeout_zero_is_reported) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    rediscoro::config cfg{};
    // Deterministic: resolve_timeout <= 0 makes with_timeout_detached immediately time out.
    cfg.host = "apple.com";
    cfg.port = 80;
    cfg.resolve_timeout = 0ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto ec = co_await c.connect();
    if (ec != rediscoro::error::resolve_timeout) {
      diag = "expected resolve_timeout, got: " + ec.message();
      guard.reset();
      co_return;
    }

    ok = true;
    guard.reset();
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_external, timeout_error_is_reported_for_unresponsive_peer) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

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
      guard.reset();
      co_return;
    }

    // Depending on routing, we may time out at TCP connect or during the handshake read/write.
    if (!(ec == rediscoro::error::connect_timeout || ec == rediscoro::error::handshake_timeout ||
          ec == rediscoro::error::connect_failed || ec == rediscoro::error::resolve_failed ||
          ec == rediscoro::error::resolve_timeout || ec == rediscoro::error::connection_reset)) {
      diag = "expected timeout/connect failure, got: " + ec.message();
      guard.reset();
      co_return;
    }
    ok = true;
    guard.reset();
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

