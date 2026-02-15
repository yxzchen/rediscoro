#include <gtest/gtest.h>

#include <rediscoro/client.hpp>
#include <rediscoro/config.hpp>

#include <iocoro/iocoro.hpp>

#include <chrono>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

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
    if (resp.get<0>().error().code != rediscoro::client_errc::not_connected) {
      diag = "expected not_connected, got: " + resp.get<0>().error().to_string();
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
    // Deterministic: resolve_timeout = 0ms makes with_timeout immediately time out.
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
    if (r.error().code != rediscoro::client_errc::resolve_timeout) {
      diag = "expected resolve_timeout, got: " + r.error().to_string();
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
    cfg.connect_timeout = 50ms;
    cfg.resolve_timeout = 50ms;
    cfg.request_timeout = 50ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto r = co_await c.connect();
    if (r.has_value()) {
      diag = "unexpected success connecting to blackhole address";
      co_return;
    }

    auto const e = r.error();

    // Depending on routing, we may time out at TCP connect or during the handshake read/write.
    if (!(e.code == rediscoro::client_errc::connect_timeout ||
          e.code == rediscoro::client_errc::handshake_timeout ||
          e.code == rediscoro::client_errc::connect_failed ||
          e.code == rediscoro::client_errc::resolve_failed ||
          e.code == rediscoro::client_errc::resolve_timeout ||
          e.code == rediscoro::client_errc::connection_reset ||
          e.code == rediscoro::client_errc::operation_aborted)) {
      diag = "expected timeout/connect failure, got: " + e.to_string();
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
      skip_reason = "redis not available at 127.0.0.1:6379 (" + r.error().to_string() + ")";
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

TEST(client_test, initial_connect_failure_emits_disconnected_event) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  struct event_probe {
    std::atomic<int> total_count{0};
    std::atomic<int> disconnected_count{0};
    static void on_event(void* user_data, rediscoro::connection_event const& ev) {
      auto* self = static_cast<event_probe*>(user_data);
      self->total_count.fetch_add(1, std::memory_order_relaxed);
      if (ev.kind == rediscoro::connection_event_kind::disconnected) {
        self->disconnected_count.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  event_probe probe{};

  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    rediscoro::config cfg{};
    cfg.host = "qq.com";
    cfg.port = 80;
    cfg.resolve_timeout = 0ms;
    cfg.reconnection.enabled = true;
    cfg.connection_hooks = {
      .user_data = &probe,
      .on_event = &event_probe::on_event,
    };

    rediscoro::client c{ctx.get_executor(), cfg};
    auto r = co_await c.connect();
    if (r.has_value()) {
      diag = "expected connect failure";
      co_return;
    }
    if (probe.disconnected_count.load(std::memory_order_relaxed) <= 0) {
      diag = "expected disconnected event on initial connect failure";
      co_return;
    }
    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_test, concurrent_exec_submission_and_state_reads_are_thread_safe) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = 6379;
  cfg.reconnection.enabled = false;

  rediscoro::client c{ctx.get_executor(), cfg};

  constexpr int kSubmitThreads = 4;
  constexpr int kRequestsPerThread = 100;
  constexpr int kTotal = kSubmitThreads * kRequestsPerThread;

  std::atomic<int> completed{0};
  std::atomic<int> rejected{0};

  std::thread runner([&] { (void)ctx.run(); });

  std::vector<std::thread> producers;
  producers.reserve(kSubmitThreads);
  for (int t = 0; t < kSubmitThreads; ++t) {
    producers.emplace_back([&, t] {
      for (int i = 0; i < kRequestsPerThread; ++i) {
        iocoro::co_spawn(
          ctx.get_executor(),
          [&]() -> iocoro::awaitable<void> {
            auto resp = co_await c.exec<std::string>("PING");
            auto& slot = resp.get<0>();
            if (!slot && slot.error().code == rediscoro::client_errc::not_connected) {
              rejected.fetch_add(1, std::memory_order_relaxed);
            }

            const auto done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
            if (done == kTotal) {
              guard.reset();
            }
            co_return;
          }(),
          iocoro::detached);

        (void)c.is_connected();
      }
    });
  }

  for (auto& th : producers) {
    th.join();
  }
  runner.join();

  ASSERT_EQ(completed.load(std::memory_order_relaxed), kTotal);
  ASSERT_EQ(rejected.load(std::memory_order_relaxed), kTotal);
  ASSERT_FALSE(c.is_connected());
}
