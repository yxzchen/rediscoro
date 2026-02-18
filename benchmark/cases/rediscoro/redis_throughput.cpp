#include <rediscoro/rediscoro.hpp>

#include <iocoro/iocoro.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

struct bench_state {
  iocoro::io_context* ctx = nullptr;
  std::atomic<int> remaining_sessions{0};
  std::atomic<bool> failed{false};
  std::uint64_t ops_per_session = 0;
  std::uint64_t inflight = 1;
  std::string host{"127.0.0.1"};
  int port = 6379;
};

void mark_done(bench_state* st) {
  if (st->remaining_sessions.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    st->ctx->stop();
  }
}

void fail_and_stop(bench_state* st, std::string message) {
  if (!st->failed.exchange(true, std::memory_order_acq_rel)) {
    std::cerr << message << "\n";
  }
  st->ctx->stop();
}

auto run_session(int session_id, bench_state* st) -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;

  rediscoro::config cfg{};
  cfg.host = st->host;
  cfg.port = st->port;
  cfg.reconnection.enabled = false;

  rediscoro::client c{ex, cfg};
  auto cr = co_await c.connect();
  if (!cr) {
    fail_and_stop(st, "rediscoro_redis_throughput: connect failed for session " +
                        std::to_string(session_id) + ": " + cr.error().to_string());
    co_return;
  }

  std::uint64_t remaining = st->ops_per_session;
  while (remaining > 0) {
    auto const batch = std::min<std::uint64_t>(remaining, st->inflight);

    rediscoro::request req{};
    for (std::uint64_t i = 0; i < batch; ++i) {
      req.push("PING");
    }

    auto resp = co_await c.exec_dynamic<rediscoro::ignore_t>(std::move(req));
    if (resp.size() != batch) {
      fail_and_stop(st,
                    "rediscoro_redis_throughput: response size mismatch for session " +
                      std::to_string(session_id));
      co_return;
    }
    for (std::size_t i = 0; i < resp.size(); ++i) {
      if (!resp[i]) {
        fail_and_stop(st,
                      "rediscoro_redis_throughput: PING failed for session " +
                        std::to_string(session_id) + ": " + resp[i].error().to_string());
        co_return;
      }
    }

    remaining -= batch;
  }

  co_await c.close();
  mark_done(st);
}

}  // namespace

int main(int argc, char* argv[]) {
  int sessions = 1;
  std::uint64_t total_ops_per_session = 200000;
  std::uint64_t inflight = 1;
  std::string host = "127.0.0.1";
  int port = 6379;

  if (argc >= 4) {
    sessions = std::stoi(argv[1]);
    total_ops_per_session = static_cast<std::uint64_t>(std::stoull(argv[2]));
    inflight = static_cast<std::uint64_t>(std::stoull(argv[3]));
  }
  if (argc >= 5) {
    host = argv[4];
  }
  if (argc >= 6) {
    port = std::stoi(argv[5]);
  }

  if (sessions <= 0) {
    std::cerr << "rediscoro_redis_throughput: sessions must be > 0\n";
    return 1;
  }
  if (total_ops_per_session == 0) {
    std::cerr << "rediscoro_redis_throughput: total_ops_per_session must be > 0\n";
    return 1;
  }
  if (inflight == 0) {
    std::cerr << "rediscoro_redis_throughput: inflight must be > 0\n";
    return 1;
  }

  iocoro::io_context ctx;

  bench_state st{};
  st.ctx = &ctx;
  st.remaining_sessions.store(sessions, std::memory_order_release);
  st.ops_per_session = total_ops_per_session;
  st.inflight = inflight;
  st.host = std::move(host);
  st.port = port;

  auto guard = iocoro::make_work_guard(ctx);
  auto ex = ctx.get_executor();

  for (int i = 0; i < sessions; ++i) {
    iocoro::co_spawn(ex, run_session(i, &st), iocoro::detached);
  }

  auto const total_ops =
    static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(total_ops_per_session);

  auto const start = std::chrono::steady_clock::now();
  ctx.run();
  auto const end = std::chrono::steady_clock::now();

  if (st.failed.load(std::memory_order_acquire)) {
    return 1;
  }
  if (st.remaining_sessions.load(std::memory_order_acquire) != 0) {
    std::cerr << "rediscoro_redis_throughput: incomplete run (remaining_sessions="
              << st.remaining_sessions.load(std::memory_order_relaxed) << ")\n";
    return 1;
  }

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const throughput_ops_s =
    elapsed_s > 0.0 ? static_cast<double>(total_ops) / elapsed_s : 0.0;
  auto const avg_session_ms =
    sessions > 0 && elapsed_s > 0.0 ? (elapsed_s * 1000.0) / static_cast<double>(sessions) : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "rediscoro_redis_throughput"
            << " host=" << st.host
            << " port=" << st.port
            << " sessions=" << sessions
            << " total_ops_per_session=" << total_ops_per_session
            << " inflight=" << inflight
            << " total_ops=" << total_ops
            << " elapsed_s=" << elapsed_s
            << " throughput_ops_s=" << throughput_ops_s
            << " avg_session_ms=" << avg_session_ms
            << "\n";

  return 0;
}
