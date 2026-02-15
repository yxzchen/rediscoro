#include <gtest/gtest.h>

#include <rediscoro/client.hpp>
#include <rediscoro/config.hpp>
#include <rediscoro/error.hpp>

#include <iocoro/co_sleep.hpp>
#include <iocoro/iocoro.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "support/fake_redis_server.hpp"

using namespace std::chrono_literals;

namespace {

using fake_server = rediscoro::test_support::fake_redis_server;

struct event_recorder {
  mutable std::mutex mu;
  std::vector<rediscoro::connection_event> events;

  static auto on_event(void* user_data, rediscoro::connection_event const& ev) -> void {
    auto* self = static_cast<event_recorder*>(user_data);
    std::lock_guard lock(self->mu);
    self->events.push_back(ev);
  }

  [[nodiscard]] auto snapshot() const -> std::vector<rediscoro::connection_event> {
    std::lock_guard lock(mu);
    return events;
  }
};

[[nodiscard]] auto count_kind(std::vector<rediscoro::connection_event> const& events,
                              rediscoro::connection_event_kind kind) -> int {
  int out = 0;
  for (auto const& ev : events) {
    if (ev.kind == kind) {
      out += 1;
    }
  }
  return out;
}

[[nodiscard]] auto find_first_error(std::vector<rediscoro::connection_event> const& events,
                                    rediscoro::connection_event_kind kind)
  -> rediscoro::error_info {
  for (auto const& ev : events) {
    if (ev.kind == kind) {
      return ev.error;
    }
  }
  return {};
}

[[nodiscard]] auto make_cfg(std::uint16_t port, event_recorder* recorder) -> rediscoro::config {
  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = static_cast<int>(port);
  cfg.resolve_timeout = 300ms;
  cfg.connect_timeout = 300ms;
  cfg.request_timeout = 300ms;
  cfg.connection_hooks = {
    .user_data = recorder,
    .on_event = &event_recorder::on_event,
  };
  return cfg;
}

}  // namespace

TEST(client_lifecycle_test, connect_close_emits_connected_then_closed) {
  fake_server server({
    {
      fake_server::action::read(),
      fake_server::action::write("+OK\r\n"),
      fake_server::action::sleep_for(50ms),
    },
  });

  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  event_recorder recorder{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    auto cfg = make_cfg(server.port(), &recorder);
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto r = co_await c.connect();
    if (!r) {
      diag = "connect failed: " + r.error().to_string();
      co_return;
    }

    co_await c.close();

    auto events = recorder.snapshot();
    if (events.size() < 2) {
      diag = "expected at least connected+closed events";
      co_return;
    }

    if (events[0].kind != rediscoro::connection_event_kind::connected) {
      diag = "first event is not connected";
      co_return;
    }

    if (events.back().kind != rediscoro::connection_event_kind::closed) {
      diag = "last event is not closed";
      co_return;
    }

    if (events[0].generation == 0) {
      diag = "connected generation should be > 0";
      co_return;
    }

    if (events.back().generation != events[0].generation) {
      diag = "closed generation should match latest connected generation";
      co_return;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(server.failure_message().empty()) << server.failure_message();
  ASSERT_TRUE(ok) << diag;
}

TEST(client_lifecycle_test, initial_connect_failure_emits_disconnected_and_returns_error) {
  fake_server server({
    {
      fake_server::action::read(),
      fake_server::action::sleep_for(300ms),
      fake_server::action::close_client(),
    },
  });

  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  event_recorder recorder{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    auto cfg = make_cfg(server.port(), &recorder);
    cfg.request_timeout = 50ms;
    cfg.reconnection.enabled = true;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto r = co_await c.connect();
    if (r.has_value()) {
      diag = "expected connect failure";
      co_return;
    }

    auto events = recorder.snapshot();
    if (count_kind(events, rediscoro::connection_event_kind::disconnected) <= 0) {
      diag = "expected disconnected event";
      co_return;
    }

    auto first_disc = find_first_error(events, rediscoro::connection_event_kind::disconnected);
    if (first_disc.code != r.error().code) {
      diag = "disconnected event error does not match connect() error";
      co_return;
    }

    co_await c.close();

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(server.failure_message().empty()) << server.failure_message();
  ASSERT_TRUE(ok) << diag;
}

TEST(client_lifecycle_test, runtime_disconnect_triggers_reconnect_then_connected_again) {
  fake_server::session_script s1{
    fake_server::action::read(),
    fake_server::action::write("+OK\r\n"),
    fake_server::action::read(),
    fake_server::action::close_client(),
  };

  fake_server::session_script s2{
    fake_server::action::read(1, 1000ms),
    fake_server::action::write("+OK\r\n"),
  };
  for (int i = 0; i < 8; ++i) {
    s2.push_back(fake_server::action::read(1, 200ms));
    s2.push_back(fake_server::action::write("+PONG\r\n"));
  }

  fake_server server({std::move(s1), std::move(s2)});

  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  event_recorder recorder{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    auto cfg = make_cfg(server.port(), &recorder);
    cfg.resolve_timeout = 1000ms;
    cfg.connect_timeout = 1000ms;
    cfg.request_timeout = 1000ms;
    cfg.reconnection.enabled = true;
    cfg.reconnection.immediate_attempts = 1;
    cfg.reconnection.initial_delay = 10ms;
    cfg.reconnection.max_delay = 20ms;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto cr = co_await c.connect();
    if (!cr) {
      diag = "initial connect failed: " + cr.error().to_string();
      co_return;
    }

    auto first = co_await c.exec<std::string>("PING");
    if (first.get<0>().has_value()) {
      diag = "first PING unexpectedly succeeded; disconnect injection failed";
      co_return;
    }

    bool recovered = false;
    for (int i = 0; i < 50; ++i) {
      auto resp = co_await c.exec<std::string>("PING");
      auto& slot = resp.get<0>();
      if (slot.has_value()) {
        if (*slot != "PONG") {
          diag = "expected PONG after reconnect";
          co_return;
        }
        recovered = true;
        break;
      }
      co_await iocoro::co_sleep(10ms);
    }

    if (!recovered) {
      diag = "did not recover after reconnect attempts";
      co_return;
    }

    co_await c.close();

    auto events = recorder.snapshot();
    if (count_kind(events, rediscoro::connection_event_kind::connected) < 2) {
      diag = "expected at least two connected events";
      co_return;
    }
    if (count_kind(events, rediscoro::connection_event_kind::disconnected) < 1) {
      diag = "expected at least one disconnected event";
      co_return;
    }

    std::uint64_t prev_generation = 0;
    int connected_seen = 0;
    for (auto const& ev : events) {
      if (ev.kind != rediscoro::connection_event_kind::connected) {
        continue;
      }
      if (connected_seen > 0 && ev.generation <= prev_generation) {
        diag = "connected generations are not strictly increasing";
        co_return;
      }
      prev_generation = ev.generation;
      connected_seen += 1;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(server.failure_message().empty()) << server.failure_message();
  ASSERT_TRUE(ok) << diag;
}

TEST(client_lifecycle_test, runtime_disconnect_with_reconnect_disabled_ends_in_closed) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  event_recorder recorder{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    auto cfg = make_cfg(6379, &recorder);
    cfg.reconnection.enabled = false;

    rediscoro::client victim{ctx.get_executor(), cfg};
    auto cr = co_await victim.connect();
    if (!cr) {
      diag = "connect failed: " + cr.error().to_string();
      co_return;
    }

    auto id_resp = co_await victim.exec<std::int64_t>("CLIENT", "ID");
    auto& id_slot = id_resp.get<0>();
    if (!id_slot) {
      diag = "CLIENT ID failed: " + id_slot.error().to_string();
      co_return;
    }
    const std::int64_t victim_id = *id_slot;

    rediscoro::config admin_cfg = cfg;
    admin_cfg.connection_hooks = {};
    rediscoro::client admin{ctx.get_executor(), admin_cfg};
    auto acr = co_await admin.connect();
    if (!acr) {
      diag = "admin connect failed: " + acr.error().to_string();
      co_return;
    }

    auto kill_resp = co_await admin.exec<std::int64_t>("CLIENT", "KILL", "ID", victim_id);
    auto& kill_slot = kill_resp.get<0>();
    if (!kill_slot) {
      diag = "CLIENT KILL failed: " + kill_slot.error().to_string();
      co_return;
    }
    if (*kill_slot < 1) {
      diag = "CLIENT KILL did not close victim connection";
      co_return;
    }
    co_await admin.close();

    bool first_failed = false;
    for (int i = 0; i < 3; ++i) {
      auto first = co_await victim.exec<std::string>("PING");
      if (!first.get<0>().has_value()) {
        first_failed = true;
        break;
      }
      co_await iocoro::co_sleep(50ms);
    }
    if (!first_failed) {
      diag = "expected PING to fail after CLIENT KILL";
      co_return;
    }

    co_await iocoro::co_sleep(60ms);

    auto second = co_await victim.exec<std::string>("PING");
    if (second.get<0>().has_value()) {
      diag = "expected second PING to fail with reconnect disabled";
      co_return;
    }

    auto events = recorder.snapshot();
    if (count_kind(events, rediscoro::connection_event_kind::disconnected) < 1) {
      diag = "expected disconnected event";
      co_return;
    }
    if (count_kind(events, rediscoro::connection_event_kind::closed) < 1) {
      diag = "expected closed event";
      co_return;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_lifecycle_test, unsolicited_message_causes_disconnected) {
  fake_server server({
    {
      fake_server::action::read(),
      fake_server::action::write("+OK\r\n"),
      fake_server::action::sleep_for(20ms),
      fake_server::action::write(">2\r\n+pubsub\r\n+message\r\n"),
      fake_server::action::sleep_for(20ms),
      fake_server::action::close_client(),
    },
  });

  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  event_recorder recorder{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    auto cfg = make_cfg(server.port(), &recorder);
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto cr = co_await c.connect();
    if (!cr) {
      diag = "connect failed: " + cr.error().to_string();
      co_return;
    }

    co_await iocoro::co_sleep(120ms);

    auto resp = co_await c.exec<std::string>("PING");
    if (resp.get<0>().has_value()) {
      diag = "expected PING to fail after unsolicited message";
      co_return;
    }

    auto events = recorder.snapshot();
    bool saw_unsolicited = false;
    for (auto const& ev : events) {
      if (ev.kind == rediscoro::connection_event_kind::disconnected &&
          ev.error.code == rediscoro::client_errc::unsolicited_message) {
        saw_unsolicited = true;
        break;
      }
    }

    if (!saw_unsolicited) {
      diag = "did not observe disconnected(unsolicited_message) event";
      co_return;
    }

    co_await c.close();

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(server.failure_message().empty()) << server.failure_message();
  ASSERT_TRUE(ok) << diag;
}

TEST(client_lifecycle_test, request_timeout_fails_inflight_and_emits_disconnected) {
  fake_server server({
    {
      fake_server::action::read(),
      fake_server::action::write("+OK\r\n"),
      fake_server::action::read(),
      fake_server::action::sleep_for(300ms),
      fake_server::action::close_client(),
    },
  });

  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  event_recorder recorder{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    auto cfg = make_cfg(server.port(), &recorder);
    cfg.request_timeout = 40ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto cr = co_await c.connect();
    if (!cr) {
      diag = "connect failed: " + cr.error().to_string();
      co_return;
    }

    auto resp = co_await c.exec<std::string>("PING");
    auto& slot = resp.get<0>();
    if (slot.has_value()) {
      diag = "expected timeout failure";
      co_return;
    }
    if (slot.error().code != rediscoro::client_errc::request_timeout) {
      diag = "expected request_timeout, got: " + slot.error().to_string();
      co_return;
    }

    auto events = recorder.snapshot();
    bool saw_timeout_disc = false;
    for (auto const& ev : events) {
      if (ev.kind == rediscoro::connection_event_kind::disconnected &&
          ev.error.code == rediscoro::client_errc::request_timeout) {
        saw_timeout_disc = true;
        break;
      }
    }

    if (!saw_timeout_disc) {
      diag = "missing disconnected(request_timeout) event";
      co_return;
    }

    co_await c.close();

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(server.failure_message().empty()) << server.failure_message();
  ASSERT_TRUE(ok) << diag;
}

TEST(client_lifecycle_test, close_is_idempotent_under_inflight_requests) {
  fake_server::session_script session{
    fake_server::action::read(),
    fake_server::action::write("+OK\r\n"),
  };
  for (int i = 0; i < 6; ++i) {
    session.push_back(fake_server::action::read());
    session.push_back(fake_server::action::sleep_for(150ms));
  }

  fake_server server({std::move(session)});

  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  event_recorder recorder{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    auto cfg = make_cfg(server.port(), &recorder);
    cfg.reconnection.enabled = false;
    cfg.request_timeout = std::nullopt;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto cr = co_await c.connect();
    if (!cr) {
      diag = "connect failed: " + cr.error().to_string();
      co_return;
    }

    rediscoro::request req{};
    req.push("PING");
    req.push("PING");
    req.push("PING");

    auto waiter = iocoro::co_spawn(ctx.get_executor(), c.exec_dynamic<std::string>(std::move(req)),
                                   iocoro::use_awaitable);

    co_await iocoro::co_sleep(20ms);
    co_await c.close();
    co_await c.close();

    auto resp = co_await waiter;
    if (resp.size() != 3) {
      diag = "expected 3 slots in dynamic response";
      co_return;
    }

    for (std::size_t i = 0; i < resp.size(); ++i) {
      if (resp[i].has_value()) {
        diag = "expected all inflight slots to fail on close";
        co_return;
      }
      if (resp[i].error().code != rediscoro::client_errc::connection_closed) {
        diag = "expected connection_closed for inflight slot";
        co_return;
      }
    }

    auto events = recorder.snapshot();
    if (count_kind(events, rediscoro::connection_event_kind::closed) != 1) {
      diag = "expected exactly one closed event";
      co_return;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(server.failure_message().empty()) << server.failure_message();
  ASSERT_TRUE(ok) << diag;
}
