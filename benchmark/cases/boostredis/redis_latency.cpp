#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/consign.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/redis.hpp>
#include <boost/redis/src.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

namespace asio = boost::asio;
namespace redis = boost::redis;

struct bench_state {
  asio::io_context* ctx = nullptr;
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

auto run_session(bench_state* st) -> asio::awaitable<void> {
  auto ex = co_await asio::this_coro::executor;

  auto conn = std::make_shared<redis::connection>(
    ex, redis::logger{redis::logger::level::disabled});
  redis::config cfg{};
  cfg.addr.host = st->host;
  cfg.addr.port = std::to_string(st->port);
  cfg.health_check_interval = std::chrono::seconds{0};
  cfg.reconnect_wait_interval = std::chrono::seconds{0};

  conn->async_run(cfg, asio::consign(asio::detached, conn));

  std::vector<double> local_latencies{};
  local_latencies.reserve(static_cast<std::size_t>(st->msgs_per_session));

  for (int i = 0; i < st->msgs_per_session; ++i) {
    redis::request req;
    req.push("ECHO", st->payload);

    auto const start = std::chrono::steady_clock::now();
    auto [ec, ignored] =
      co_await conn->async_exec(req, redis::ignore, asio::as_tuple(asio::use_awaitable));
    (void)ignored;
    if (ec) {
      fail_and_stop(st, "boostredis_redis_latency: ECHO failed: " + ec.message());
      conn->cancel();
      co_return;
    }

    auto const end = std::chrono::steady_clock::now();
    auto const us = std::chrono::duration<double, std::micro>(end - start).count();
    local_latencies.push_back(us);
  }

  conn->cancel();

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
    std::cerr << "boostredis_redis_latency: sessions must be > 0\n";
    return 1;
  }
  if (msgs <= 0) {
    std::cerr << "boostredis_redis_latency: msgs must be > 0\n";
    return 1;
  }

  asio::io_context ctx;

  bench_state st{};
  st.ctx = &ctx;
  st.remaining_sessions.store(sessions, std::memory_order_release);
  st.msgs_per_session = msgs;
  st.host = std::move(host);
  st.port = port;
  st.payload.assign(msg_bytes, 'x');
  st.latencies_us.reserve(static_cast<std::size_t>(sessions) * static_cast<std::size_t>(msgs));

  for (int i = 0; i < sessions; ++i) {
    asio::co_spawn(ctx, run_session(&st), asio::detached);
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
    std::cerr << "boostredis_redis_latency: incomplete run (remaining_sessions="
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
    std::cerr << "boostredis_redis_latency: sample mismatch (expected=" << expected_samples
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
  std::cout << "boostredis_redis_latency"
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
