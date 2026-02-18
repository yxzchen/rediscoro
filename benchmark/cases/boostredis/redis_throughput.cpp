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
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>

namespace {

namespace asio = boost::asio;
namespace redis = boost::redis;

struct bench_state {
  asio::io_context* ctx = nullptr;
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

auto run_session(int session_id, bench_state* st) -> asio::awaitable<void> {
  auto ex = co_await asio::this_coro::executor;

  auto conn = std::make_shared<redis::connection>(
    ex, redis::logger{redis::logger::level::disabled});
  redis::config cfg{};
  cfg.addr.host = st->host;
  cfg.addr.port = std::to_string(st->port);
  cfg.health_check_interval = std::chrono::seconds{0};
  cfg.reconnect_wait_interval = std::chrono::seconds{0};

  conn->async_run(cfg, asio::consign(asio::detached, conn));

  std::uint64_t remaining = st->ops_per_session;
  while (remaining > 0) {
    auto const batch = std::min<std::uint64_t>(remaining, st->inflight);

    redis::request req;
    for (std::uint64_t i = 0; i < batch; ++i) {
      req.push("PING");
    }

    redis::generic_response resp{};
    auto [ec, ignored] =
      co_await conn->async_exec(req, resp, asio::as_tuple(asio::use_awaitable));
    (void)ignored;
    if (ec) {
      fail_and_stop(st, "boostredis_redis_throughput: PING failed for session " +
                          std::to_string(session_id) + ": " + ec.message());
      conn->cancel();
      co_return;
    }

    remaining -= batch;
  }

  conn->cancel();
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
    std::cerr << "boostredis_redis_throughput: sessions must be > 0\n";
    return 1;
  }
  if (total_ops_per_session == 0) {
    std::cerr << "boostredis_redis_throughput: total_ops_per_session must be > 0\n";
    return 1;
  }
  if (inflight == 0) {
    std::cerr << "boostredis_redis_throughput: inflight must be > 0\n";
    return 1;
  }

  asio::io_context ctx;

  bench_state st{};
  st.ctx = &ctx;
  st.remaining_sessions.store(sessions, std::memory_order_release);
  st.ops_per_session = total_ops_per_session;
  st.inflight = inflight;
  st.host = std::move(host);
  st.port = port;

  for (int i = 0; i < sessions; ++i) {
    asio::co_spawn(ctx, run_session(i, &st), asio::detached);
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
    std::cerr << "boostredis_redis_throughput: incomplete run (remaining_sessions="
              << st.remaining_sessions.load(std::memory_order_relaxed) << ")\n";
    return 1;
  }

  auto const elapsed_s = std::chrono::duration<double>(end - start).count();
  auto const throughput_ops_s =
    elapsed_s > 0.0 ? static_cast<double>(total_ops) / elapsed_s : 0.0;
  auto const avg_session_ms =
    sessions > 0 && elapsed_s > 0.0 ? (elapsed_s * 1000.0) / static_cast<double>(sessions) : 0.0;

  std::cout << std::fixed << std::setprecision(2);
  std::cout << "boostredis_redis_throughput"
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
