#include <rediscoro/rediscoro.hpp>

#include <iocoro/iocoro.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
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
  int msgs_per_session = 0;
  std::string host{"127.0.0.1"};
  int port = 6379;
  std::string payload{};
  std::mutex latency_mtx{};
  std::vector<double> latencies_us{};
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

auto run_session(bench_state* st) -> iocoro::awaitable<void> {
  auto ex = co_await iocoro::this_coro::executor;

  rediscoro::config cfg{};
  cfg.host = st->host;
  cfg.port = st->port;
  cfg.reconnection.enabled = false;

  rediscoro::client c{ex, cfg};
  auto cr = co_await c.connect();
  if (!cr) {
    fail_and_stop(st, "rediscoro_redis_latency: connect failed: " + cr.error().to_string());
    co_return;
  }

  std::vector<double> local_latencies{};
  local_latencies.reserve(static_cast<std::size_t>(st->msgs_per_session));

  for (int i = 0; i < st->msgs_per_session; ++i) {
    auto const start = std::chrono::steady_clock::now();
    auto resp = co_await c.exec<rediscoro::ignore_t>("ECHO", st->payload);
    if (!resp.get<0>()) {
      fail_and_stop(st, "rediscoro_redis_latency: ECHO failed: " + resp.get<0>().error().to_string());
      co_return;
    }

    auto const end = std::chrono::steady_clock::now();
    auto const us = std::chrono::duration<double, std::micro>(end - start).count();
    local_latencies.push_back(us);
  }

  co_await c.close();

  {
    std::scoped_lock lk{st->latency_mtx};
    st->latencies_us.insert(st->latencies_us.end(), local_latencies.begin(), local_latencies.end());
  }
  mark_done(st);
}

}  // namespace

int main(int argc, char* argv[]) {
  int sessions = 1;
  int msgs = 5000;
  std::size_t msg_bytes = 16;
  std::string host = "127.0.0.1";
  int port = 6379;

  if (argc >= 3) {
    sessions = std::stoi(argv[1]);
    msgs = std::stoi(argv[2]);
  }
  if (argc >= 4) {
    msg_bytes = static_cast<std::size_t>(std::stoull(argv[3]));
  }
  if (argc >= 5) {
    host = argv[4];
  }
  if (argc >= 6) {
    port = std::stoi(argv[5]);
  }

  if (sessions <= 0) {
    std::cerr << "rediscoro_redis_latency: sessions must be > 0\n";
    return 1;
  }
  if (msgs <= 0) {
    std::cerr << "rediscoro_redis_latency: msgs must be > 0\n";
    return 1;
  }

  iocoro::io_context ctx;

  bench_state st{};
  st.ctx = &ctx;
  st.remaining_sessions.store(sessions, std::memory_order_release);
  st.msgs_per_session = msgs;
  st.host = std::move(host);
  st.port = port;
  st.payload.assign(msg_bytes, 'x');
  st.latencies_us.reserve(static_cast<std::size_t>(sessions) * static_cast<std::size_t>(msgs));

  auto guard = iocoro::make_work_guard(ctx);
  auto ex = ctx.get_executor();

  for (int i = 0; i < sessions; ++i) {
    iocoro::co_spawn(ex, run_session(&st), iocoro::detached);
  }

  auto const expected_samples =
    static_cast<std::uint64_t>(sessions) * static_cast<std::uint64_t>(msgs);

  auto const start = std::chrono::steady_clock::now();
  ctx.run();
  auto const end = std::chrono::steady_clock::now();

  if (st.failed.load(std::memory_order_acquire)) {
    return 1;
  }

  if (st.remaining_sessions.load(std::memory_order_acquire) != 0) {
    std::cerr << "rediscoro_redis_latency: incomplete run (remaining_sessions="
              << st.remaining_sessions.load(std::memory_order_relaxed) << ")\n";
    return 1;
  }

  std::vector<double> samples{};
  {
    std::scoped_lock lk{st.latency_mtx};
    samples = st.latencies_us;
  }

  auto const sample_count = static_cast<std::uint64_t>(samples.size());
  if (sample_count != expected_samples) {
    std::cerr << "rediscoro_redis_latency: sample mismatch (expected=" << expected_samples
              << ", got=" << sample_count << ")\n";
    return 1;
  }

  std::sort(samples.begin(), samples.end());

  double total_us = 0.0;
  for (auto const v : samples) {
    total_us += v;
  }

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const p50_us = percentile_sorted(samples, 0.50);
  auto const p95_us = percentile_sorted(samples, 0.95);
  auto const p99_us = percentile_sorted(samples, 0.99);
  auto const avg_us = sample_count > 0 ? total_us / static_cast<double>(sample_count) : 0.0;
  auto const rps = elapsed_s > 0.0 ? static_cast<double>(sample_count) / elapsed_s : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "rediscoro_redis_latency"
            << " host=" << st.host
            << " port=" << st.port
            << " sessions=" << sessions
            << " msgs=" << msgs
            << " msg_bytes=" << msg_bytes
            << " samples=" << sample_count
            << " elapsed_s=" << elapsed_s
            << " rps=" << rps
            << " avg_us=" << avg_us
            << " p50_us=" << p50_us
            << " p95_us=" << p95_us
            << " p99_us=" << p99_us
            << "\n";

  return 0;
}
