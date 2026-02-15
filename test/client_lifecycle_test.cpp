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

using namespace std::chrono_literals;

namespace {

constexpr int kRedisPort = 6379;

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

[[nodiscard]] auto make_cfg(int port, event_recorder* recorder) -> rediscoro::config {
  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = port;
  cfg.resolve_timeout = 1s;
  cfg.connect_timeout = 1s;
  cfg.request_timeout = 1s;
  cfg.connection_hooks = {
    .user_data = recorder,
    .on_event = &event_recorder::on_event,
  };
  return cfg;
}

auto connect_with_retry(rediscoro::client& c, int attempts = 8,
                        std::chrono::milliseconds backoff = 50ms)
  -> iocoro::awaitable<rediscoro::expected<void, rediscoro::error_info>> {
  rediscoro::error_info last{};
  for (int i = 0; i < attempts; ++i) {
    auto r = co_await c.connect();
    if (r) {
      co_return r;
    }
    last = r.error();
    if (i + 1 < attempts) {
      co_await iocoro::co_sleep(backoff);
    }
  }
  co_return rediscoro::unexpected(std::move(last));
}

[[nodiscard]] auto unique_key_suffix() -> std::string {
  return std::to_string(
    static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
}

}  // namespace

TEST(client_lifecycle_test, connect_close_emits_connected_then_closed) {
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

    auto cfg = make_cfg(kRedisPort, &recorder);
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    bool pass = false;
    do {
      auto r = co_await c.connect();
      if (!r) {
        diag = "connect failed: " + r.error().to_string();
        break;
      }

      co_await c.close();

      auto events = recorder.snapshot();
      if (events.size() < 2) {
        diag = "expected at least connected+closed events";
        break;
      }
      if (events.back().kind != rediscoro::connection_event_kind::closed) {
        diag = "last event is not closed";
        break;
      }
      std::uint64_t last_connected_gen = 0;
      for (auto const& ev : events) {
        if (ev.kind == rediscoro::connection_event_kind::connected) {
          last_connected_gen = ev.generation;
        }
      }
      if (last_connected_gen == 0) {
        diag = "missing connected event";
        break;
      }
      if (events.back().generation != last_connected_gen) {
        diag = "closed generation should match last connected generation";
        break;
      }

      pass = true;
    } while (false);

    if (pass) {
      ok = true;
    }
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_lifecycle_test, initial_connect_failure_emits_disconnected_and_returns_error) {
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

    auto cfg = make_cfg(1, &recorder);
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

  ASSERT_TRUE(ok) << diag;
}

TEST(client_lifecycle_test, runtime_disconnect_triggers_reconnect_then_connected_again) {
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
    auto cfg = make_cfg(kRedisPort, &recorder);
    cfg.reconnection.enabled = true;
    cfg.reconnection.immediate_attempts = 1;
    cfg.reconnection.initial_delay = 10ms;
    cfg.reconnection.max_delay = 20ms;

    rediscoro::config admin_cfg = cfg;
    admin_cfg.connection_hooks = {};
    rediscoro::client c{ctx.get_executor(), cfg};
    rediscoro::client admin{ctx.get_executor(), admin_cfg};

    bool pass = false;
    do {
      auto cr = co_await connect_with_retry(c);
      if (!cr) {
        diag = "initial connect failed: " + cr.error().to_string();
        break;
      }

      auto id_resp = co_await c.exec<std::int64_t>("CLIENT", "ID");
      auto& id_slot = id_resp.get<0>();
      if (!id_slot) {
        diag = "CLIENT ID failed: " + id_slot.error().to_string();
        break;
      }
      const std::int64_t victim_id = *id_slot;

      auto acr = co_await connect_with_retry(admin);
      if (!acr) {
        diag = "admin connect failed: " + acr.error().to_string();
        break;
      }

      auto kill_resp = co_await admin.exec<std::int64_t>("CLIENT", "KILL", "ID", victim_id);
      auto& kill_slot = kill_resp.get<0>();
      if (!kill_slot || *kill_slot < 1) {
        diag = "CLIENT KILL failed for reconnect test";
        break;
      }

      bool got_new_client_id = false;
      for (int i = 0; i < 80; ++i) {
        auto id2_resp = co_await c.exec<std::int64_t>("CLIENT", "ID");
        auto& id2_slot = id2_resp.get<0>();
        if (id2_slot && *id2_slot != victim_id) {
          got_new_client_id = true;
          break;
        }
        co_await iocoro::co_sleep(20ms);
      }
      if (!got_new_client_id) {
        diag = "did not observe a new CLIENT ID after reconnect";
        break;
      }

      auto ping = co_await c.exec<std::string>("PING");
      if (!ping.get<0>() || *ping.get<0>() != "PONG") {
        diag = "PING after reconnect failed";
        break;
      }

      pass = true;
    } while (false);

    co_await admin.close();
    co_await c.close();
    if (pass) {
      ok = true;
    }
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

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
    auto cfg = make_cfg(kRedisPort, &recorder);
    cfg.reconnection.enabled = false;

    rediscoro::config admin_cfg = cfg;
    admin_cfg.connection_hooks = {};
    rediscoro::client victim{ctx.get_executor(), cfg};
    rediscoro::client admin{ctx.get_executor(), admin_cfg};

    bool pass = false;
    do {
      auto cr = co_await connect_with_retry(victim);
      if (!cr) {
        diag = "connect failed: " + cr.error().to_string();
        break;
      }

      auto id_resp = co_await victim.exec<std::int64_t>("CLIENT", "ID");
      auto& id_slot = id_resp.get<0>();
      if (!id_slot) {
        diag = "CLIENT ID failed: " + id_slot.error().to_string();
        break;
      }
      const std::int64_t victim_id = *id_slot;

      auto acr = co_await connect_with_retry(admin);
      if (!acr) {
        diag = "admin connect failed: " + acr.error().to_string();
        break;
      }

      auto kill_resp = co_await admin.exec<std::int64_t>("CLIENT", "KILL", "ID", victim_id);
      auto& kill_slot = kill_resp.get<0>();
      if (!kill_slot || *kill_slot < 1) {
        diag = "CLIENT KILL did not close victim connection";
        break;
      }

      bool first_failed = false;
      for (int i = 0; i < 5; ++i) {
        auto first = co_await victim.exec<std::string>("PING");
        if (!first.get<0>().has_value()) {
          first_failed = true;
          break;
        }
        co_await iocoro::co_sleep(30ms);
      }
      if (!first_failed) {
        diag = "expected PING to fail after CLIENT KILL";
        break;
      }

      auto second = co_await victim.exec<std::string>("PING");
      if (second.get<0>().has_value()) {
        diag = "expected second PING to fail with reconnect disabled";
        break;
      }

      auto events = recorder.snapshot();
      if (count_kind(events, rediscoro::connection_event_kind::disconnected) < 1) {
        diag = "expected disconnected event";
        break;
      }
      if (count_kind(events, rediscoro::connection_event_kind::closed) < 1) {
        diag = "expected closed event";
        break;
      }

      pass = true;
    } while (false);

    co_await admin.close();
    co_await victim.close();
    if (pass) {
      ok = true;
    }
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_lifecycle_test, server_side_disconnect_causes_disconnected_event) {
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

    auto cfg = make_cfg(kRedisPort, &recorder);
    cfg.reconnection.enabled = false;

    rediscoro::config admin_cfg = cfg;
    admin_cfg.connection_hooks = {};
    rediscoro::client victim{ctx.get_executor(), cfg};
    rediscoro::client admin{ctx.get_executor(), admin_cfg};

    bool pass = false;
    do {
      auto cr = co_await connect_with_retry(victim);
      if (!cr) {
        diag = "connect failed: " + cr.error().to_string();
        break;
      }

      auto id_resp = co_await victim.exec<std::int64_t>("CLIENT", "ID");
      auto& id_slot = id_resp.get<0>();
      if (!id_slot) {
        diag = "CLIENT ID failed";
        break;
      }

      auto acr = co_await connect_with_retry(admin);
      if (!acr) {
        diag = "admin connect failed";
        break;
      }

      auto kill_resp = co_await admin.exec<std::int64_t>("CLIENT", "KILL", "ID", *id_slot);
      if (!kill_resp.get<0>() || *kill_resp.get<0>() < 1) {
        diag = "CLIENT KILL failed";
        break;
      }

      auto resp = co_await victim.exec<std::string>("PING");
      if (resp.get<0>().has_value()) {
        diag = "expected PING to fail after server-side disconnect";
        break;
      }

      auto events = recorder.snapshot();
      if (count_kind(events, rediscoro::connection_event_kind::disconnected) < 1) {
        diag = "missing disconnected event";
        break;
      }

      pass = true;
    } while (false);

    co_await admin.close();
    co_await victim.close();
    if (pass) {
      ok = true;
    }
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_lifecycle_test, request_timeout_fails_inflight_and_emits_disconnected) {
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

    auto cfg = make_cfg(kRedisPort, &recorder);
    cfg.request_timeout = 40ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto cr = co_await connect_with_retry(c);
    if (!cr) {
      diag = "connect failed: " + cr.error().to_string();
      co_return;
    }

    const std::string key = "rediscoro:test:timeout:" + unique_key_suffix();
    (void)co_await c.exec<std::int64_t>("DEL", key);

    auto resp = co_await c.exec<std::string>("BLPOP", key, "5");
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

  ASSERT_TRUE(ok) << diag;
}

TEST(client_lifecycle_test, close_is_idempotent_under_inflight_requests) {
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

    auto cfg = make_cfg(kRedisPort, &recorder);
    cfg.reconnection.enabled = false;
    cfg.request_timeout = std::nullopt;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto cr = co_await connect_with_retry(c);
    if (!cr) {
      diag = "connect failed: " + cr.error().to_string();
      co_return;
    }

    const std::string suffix = unique_key_suffix();
    rediscoro::request req{};
    req.push("BLPOP", "rediscoro:test:close:" + suffix + ":1", "5");
    req.push("BLPOP", "rediscoro:test:close:" + suffix + ":2", "5");
    req.push("BLPOP", "rediscoro:test:close:" + suffix + ":3", "5");

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
    if (count_kind(events, rediscoro::connection_event_kind::closed) < 1) {
      diag = "expected at least one closed event";
      co_return;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}
