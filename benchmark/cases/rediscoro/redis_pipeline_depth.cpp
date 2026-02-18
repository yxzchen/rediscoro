#include <rediscoro/rediscoro.hpp>

#include <iocoro/iocoro.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct bench_state {
  iocoro::io_context* ctx = nullptr;
  std::atomic<int> remaining_sessions{0};
  std::atomic<bool> failed{false};
  std::uint64_t cmds_per_session = 0;
  std::uint64_t pipeline_depth = 1;
  std::string host{"127.0.0.1"};
  int port = 6379;
  std::mutex latency_mtx{};
  std::vector<double> cmd_latencies_us{};
};

auto percentile_sorted(std::vector<double> const& sorted, double q) -> double {
  if (sorted.empty()) {
    return 0.0;
  }
  if (q <= 0.0) {
    return sorted.front();
  }
  if (q >= 1.0) {
    return sorted.back();
  }
  auto const idx = static_cast<std::size_t>(
    std::ceil(q * static_cast<double>(sorted.size() - 1)));
  return sorted[idx];
}

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
    fail_and_stop(st, "rediscoro_redis_pipeline_depth: connect failed for session " +
                        std::to_string(session_id) + ": " + cr.error().to_string());
    co_return;
  }

  std::vector<double> local_cmd_latencies{};
  local_cmd_latencies.reserve(static_cast<std::size_t>(st->cmds_per_session));

  std::uint64_t remaining = st->cmds_per_session;
  while (remaining > 0) {
    auto const batch = std::min<std::uint64_t>(remaining, st->pipeline_depth);

    rediscoro::request req{};
    for (std::uint64_t i = 0; i < batch; ++i) {
      req.push("PING");
    }

    auto const start = std::chrono::steady_clock::now();
    auto resp = co_await c.exec_dynamic<rediscoro::ignore_t>(std::move(req));
    auto const end = std::chrono::steady_clock::now();

    if (resp.size() != batch) {
      fail_and_stop(st,
                    "rediscoro_redis_pipeline_depth: response size mismatch for session " +
                      std::to_string(session_id));
      co_return;
    }
    for (std::size_t i = 0; i < resp.size(); ++i) {
      if (!resp[i]) {
        fail_and_stop(st,
                      "rediscoro_redis_pipeline_depth: PING failed for session " +
                        std::to_string(session_id) + ": " + resp[i].error().to_string());
        co_return;
      }
    }

    auto const batch_us = std::chrono::duration<double, std::micro>(end - start).count();
    auto const per_cmd_us = batch_us / static_cast<double>(batch);
    for (std::uint64_t i = 0; i < batch; ++i) {
      local_cmd_latencies.push_back(per_cmd_us);
    }

    remaining -= batch;
  }

  co_await c.close();

  {
    std::scoped_lock lk{st->latency_mtx};
    st->cmd_latencies_us.insert(
      st->cmd_latencies_us.end(), local_cmd_latencies.begin(), local_cmd_latencies.end());
  }
  mark_done(st);
}

}  // namespace

int main(int argc, char* argv[]) {
  int sessions = 8;
  std::uint64_t total_cmds_per_session = 80000;
  std::uint64_t pipeline_depth = 1;
  std::string host = "127.0.0.1";
  int port = 6379;

  if (argc >= 4) {
    sessions = std::stoi(argv[1]);
    total_cmds_per_session = static_cast<std::uint64_t>(std::stoull(argv[2]));
    pipeline_depth = static_cast<std::uint64_t>(std::stoull(argv[3]));
  }
  if (argc >= 5) {
    host = argv[4];
  }
  if (argc >= 6) {
    port = std::stoi(argv[5]);
  }

  if (sessions <= 0) {
    std::cerr << "rediscoro_redis_pipeline_depth: sessions must be > 0\n";
    return 1;
  }
  if (total_cmds_per_session == 0) {
    std::cerr << "rediscoro_redis_pipeline_depth: total_cmds_per_session must be > 0\n";
    return 1;
  }
  if (pipeline_depth == 0) {
    std::cerr << "rediscoro_redis_pipeline_depth: pipeline_depth must be > 0\n";
    return 1;
  }

  iocoro::io_context ctx;

  bench_state st{};
  st.ctx = &ctx;
  st.remaining_sessions.store(sessions, std::memory_order_release);
  st.cmds_per_session = total_cmds_per_session;
  st.pipeline_depth = pipeline_depth;
  st.host = std::move(host);
  st.port = port;
  st.cmd_latencies_us.reserve(
    static_cast<std::size_t>(sessions) * static_cast<std::size_t>(total_cmds_per_session));

  auto guard = iocoro::make_work_guard(ctx);
  auto ex = ctx.get_executor();

  for (int i = 0; i < sessions; ++i) {
    iocoro::co_spawn(ex, run_session(i, &st), iocoro::detached);
  }

  auto const total_cmds =
    static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(total_cmds_per_session);

  auto const start = std::chrono::steady_clock::now();
  ctx.run();
  auto const end = std::chrono::steady_clock::now();

  if (st.failed.load(std::memory_order_acquire)) {
    return 1;
  }
  if (st.remaining_sessions.load(std::memory_order_acquire) != 0) {
    std::cerr << "rediscoro_redis_pipeline_depth: incomplete run (remaining_sessions="
              << st.remaining_sessions.load(std::memory_order_relaxed) << ")\n";
    return 1;
  }

  std::vector<double> samples{};
  {
    std::scoped_lock lk{st.latency_mtx};
    samples = st.cmd_latencies_us;
  }

  auto const sample_count = static_cast<std::uint64_t>(samples.size());
  if (sample_count != total_cmds) {
    std::cerr << "rediscoro_redis_pipeline_depth: sample mismatch (expected=" << total_cmds
              << ", got=" << sample_count << ")\n";
    return 1;
  }

  std::sort(samples.begin(), samples.end());
  auto const p50_cmd_us = percentile_sorted(samples, 0.50);
  auto const p95_cmd_us = percentile_sorted(samples, 0.95);
  auto const p99_cmd_us = percentile_sorted(samples, 0.99);

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const throughput_ops_s =
    elapsed_s > 0.0 ? static_cast<double>(total_cmds) / elapsed_s : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "rediscoro_redis_pipeline_depth"
            << " host=" << st.host
            << " port=" << st.port
            << " sessions=" << sessions
            << " total_cmds_per_session=" << total_cmds_per_session
            << " pipeline_depth=" << pipeline_depth
            << " total_cmds=" << total_cmds
            << " elapsed_s=" << elapsed_s
            << " throughput_ops_s=" << throughput_ops_s
            << " p50_cmd_us=" << p50_cmd_us
            << " p95_cmd_us=" << p95_cmd_us
            << " p99_cmd_us=" << p99_cmd_us
            << "\n";

  return 0;
}
