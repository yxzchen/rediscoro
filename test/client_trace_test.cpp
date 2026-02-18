#include <gtest/gtest.h>

#include <rediscoro/client.hpp>
#include <rediscoro/config.hpp>
#include <rediscoro/error.hpp>

#include <iocoro/co_sleep.hpp>
#include <iocoro/iocoro.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr int kRedisPort = 6379;

struct trace_recorder {
  struct finish_snapshot {
    rediscoro::request_trace_info info{};
    std::chrono::nanoseconds duration{};
    std::size_t ok_count{0};
    std::size_t error_count{0};
    std::error_code primary_error{};
    std::string primary_error_detail{};
  };

  mutable std::mutex mu;
  std::vector<rediscoro::request_trace_start> starts;
  std::vector<finish_snapshot> finishes;

  static auto on_start(void* user_data, rediscoro::request_trace_start const& ev) -> void {
    auto* self = static_cast<trace_recorder*>(user_data);
    std::lock_guard lock(self->mu);
    self->starts.push_back(ev);
  }

  static auto on_finish(void* user_data, rediscoro::request_trace_finish const& ev) -> void {
    auto* self = static_cast<trace_recorder*>(user_data);
    finish_snapshot out{};
    out.info = ev.info;
    out.duration = ev.duration;
    out.ok_count = ev.ok_count;
    out.error_count = ev.error_count;
    out.primary_error = ev.primary_error;
    out.primary_error_detail = std::string(ev.primary_error_detail);

    std::lock_guard lock(self->mu);
    self->finishes.push_back(std::move(out));
  }

  [[nodiscard]] auto start_snapshot() const -> std::vector<rediscoro::request_trace_start> {
    std::lock_guard lock(mu);
    return starts;
  }

  [[nodiscard]] auto finish_snapshot_copy() const -> std::vector<finish_snapshot> {
    std::lock_guard lock(mu);
    return finishes;
  }
};

[[nodiscard]] auto make_cfg(trace_recorder* recorder) -> rediscoro::config {
  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = kRedisPort;
  cfg.resolve_timeout = 1s;
  cfg.connect_timeout = 1s;
  cfg.request_timeout = 1s;
  cfg.reconnection.enabled = false;
  cfg.trace_hooks = {
    .user_data = recorder,
    .on_start = &trace_recorder::on_start,
    .on_finish = &trace_recorder::on_finish,
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

}  // namespace

TEST(client_trace_test, user_request_trace_start_finish_success) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  trace_recorder recorder{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    auto cfg = make_cfg(&recorder);
    cfg.trace_handshake = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto cr = co_await connect_with_retry(c);
    if (!cr) {
      diag = "connect failed: " + cr.error().to_string();
      co_return;
    }

    auto resp = co_await c.exec<std::string>("PING");
    auto& slot = resp.get<0>();
    if (!slot || *slot != "PONG") {
      diag = "PING failed";
      co_return;
    }

    co_await c.close();

    auto starts = recorder.start_snapshot();
    auto finishes = recorder.finish_snapshot_copy();

    if (starts.size() != 1 || finishes.size() != 1) {
      diag = "expected exactly one user trace start/finish";
      co_return;
    }
    if (starts[0].info.kind != rediscoro::request_kind::user ||
        finishes[0].info.kind != rediscoro::request_kind::user) {
      diag = "expected user trace kind";
      co_return;
    }
    if (finishes[0].ok_count != 1 || finishes[0].error_count != 0) {
      diag = "unexpected finish summary for success path";
      co_return;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_trace_test, user_request_trace_finish_contains_primary_error_detail) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  trace_recorder recorder{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    auto cfg = make_cfg(&recorder);
    cfg.trace_handshake = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto cr = co_await connect_with_retry(c);
    if (!cr) {
      diag = "connect failed: " + cr.error().to_string();
      co_return;
    }

    auto resp = co_await c.exec<std::string>("THIS_COMMAND_DOES_NOT_EXIST_REDISCORO");
    auto& slot = resp.get<0>();
    if (slot.has_value()) {
      diag = "expected server error";
      co_return;
    }
    if (slot.error().code != rediscoro::server_errc::redis_error) {
      diag = "expected redis_error";
      co_return;
    }

    co_await c.close();

    auto finishes = recorder.finish_snapshot_copy();
    if (finishes.size() != 1) {
      diag = "expected exactly one finish trace";
      co_return;
    }
    if (finishes[0].error_count != 1 || finishes[0].ok_count != 0) {
      diag = "unexpected finish summary for error path";
      co_return;
    }
    if (finishes[0].primary_error != rediscoro::server_errc::redis_error) {
      diag = "primary_error mismatch";
      co_return;
    }
    if (finishes[0].primary_error_detail.empty()) {
      diag = "primary_error_detail should not be empty";
      co_return;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_trace_test, handshake_trace_emitted_only_when_enabled) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  trace_recorder rec_no_handshake{};
  trace_recorder rec_with_handshake{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    {
      auto cfg = make_cfg(&rec_no_handshake);
      cfg.trace_handshake = false;
      rediscoro::client c{ctx.get_executor(), cfg};
      auto cr = co_await connect_with_retry(c);
      if (!cr) {
        diag = "connect (trace_handshake=false) failed: " + cr.error().to_string();
        co_return;
      }
      co_await c.close();
    }

    {
      auto cfg = make_cfg(&rec_with_handshake);
      cfg.trace_handshake = true;
      rediscoro::client c{ctx.get_executor(), cfg};
      auto cr = co_await connect_with_retry(c);
      if (!cr) {
        diag = "connect (trace_handshake=true) failed: " + cr.error().to_string();
        co_return;
      }
      co_await c.close();
    }

    auto starts_a = rec_no_handshake.start_snapshot();
    auto finishes_a = rec_no_handshake.finish_snapshot_copy();
    if (!starts_a.empty() || !finishes_a.empty()) {
      diag = "trace_handshake=false should not emit handshake traces when no user request";
      co_return;
    }

    auto starts_b = rec_with_handshake.start_snapshot();
    auto finishes_b = rec_with_handshake.finish_snapshot_copy();
    if (starts_b.empty() || finishes_b.empty() || starts_b.size() != finishes_b.size()) {
      diag = "trace_handshake=true should emit handshake trace pairs";
      co_return;
    }
    for (std::size_t i = 0; i < starts_b.size(); ++i) {
      if (starts_b[i].info.kind != rediscoro::request_kind::handshake ||
          finishes_b[i].info.kind != rediscoro::request_kind::handshake) {
        diag = "expected handshake trace kind";
        co_return;
      }
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}

TEST(client_trace_test, trace_callback_throw_is_swallowed) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  struct throwing_trace_state {
    std::atomic<int> start_calls{0};
    std::atomic<int> finish_calls{0};
  };
  throwing_trace_state throw_state{};

  bool ok = false;
  std::string diag{};

  auto on_start = [](void* user_data, rediscoro::request_trace_start const&) {
    auto* state = static_cast<throwing_trace_state*>(user_data);
    state->start_calls.fetch_add(1, std::memory_order_relaxed);
    throw std::runtime_error("start callback throw");
  };

  auto on_finish = [](void* user_data, rediscoro::request_trace_finish const&) {
    auto* state = static_cast<throwing_trace_state*>(user_data);
    state->finish_calls.fetch_add(1, std::memory_order_relaxed);
    throw std::runtime_error("finish callback throw");
  };

  auto task = [&]() -> iocoro::awaitable<void> {
    struct work_guard_reset {
      decltype(guard)& g;
      ~work_guard_reset() { g.reset(); }
    };
    work_guard_reset reset{guard};

    rediscoro::config cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = kRedisPort;
    cfg.resolve_timeout = 300ms;
    cfg.connect_timeout = 300ms;
    cfg.request_timeout = 300ms;
    cfg.reconnection.enabled = false;
    cfg.trace_handshake = false;
    cfg.trace_hooks = {
      .user_data = &throw_state,
      .on_start = on_start,
      .on_finish = on_finish,
    };

    rediscoro::client c{ctx.get_executor(), cfg};
    auto cr = co_await connect_with_retry(c);
    if (!cr) {
      diag = "connect failed: " + cr.error().to_string();
      co_return;
    }

    auto resp = co_await c.exec<std::string>("PING");
    auto& slot = resp.get<0>();
    if (!slot || *slot != "PONG") {
      diag = "PING failed while callbacks throw";
      co_return;
    }

    co_await c.close();

    if (throw_state.start_calls.load(std::memory_order_relaxed) <= 0) {
      diag = "on_start should still be called";
      co_return;
    }
    if (throw_state.finish_calls.load(std::memory_order_relaxed) <= 0) {
      diag = "on_finish should still be called";
      co_return;
    }

    ok = true;
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  ASSERT_TRUE(ok) << diag;
}
