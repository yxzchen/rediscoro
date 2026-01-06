#include <gtest/gtest.h>

#include <rediscoro/client.hpp>
#include <rediscoro/config.hpp>
#include <rediscoro/ignore.hpp>

#include <iocoro/iocoro.hpp>
#include <iocoro/steady_timer.hpp>

#include <chrono>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

auto co_sleep(iocoro::io_executor ex, std::chrono::milliseconds d) -> iocoro::awaitable<void> {
  iocoro::steady_timer timer{ex};
  timer.expires_after(d);
  (void)co_await timer.async_wait(iocoro::use_awaitable);
  co_return;
}

class local_tcp_server {
public:
  enum class behavior {
    close_immediately,
    hang_after_accept,
  };

  explicit local_tcp_server(behavior b) : behavior_(b) {}

  local_tcp_server(local_tcp_server const&) = delete;
  auto operator=(local_tcp_server const&) -> local_tcp_server& = delete;

  ~local_tcp_server() {
    stop();
  }

  auto start() -> bool {
    if (started_) {
      return true;
    }

    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return false;
    }

    int one = 1;
    if (::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }

    if (::listen(fd_, 1) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
      ::close(fd_);
      fd_ = -1;
      return false;
    }
    port_ = ntohs(bound.sin_port);

    stop_requested_.store(false, std::memory_order_release);
    th_ = std::thread([this]() { run(); });
    started_ = true;
    return true;
  }

  auto stop() -> void {
    if (!started_) {
      return;
    }

    stop_requested_.store(true, std::memory_order_release);
    if (fd_ >= 0) {
      ::shutdown(fd_, SHUT_RDWR);
      ::close(fd_);
      fd_ = -1;
    }
    if (th_.joinable()) {
      th_.join();
    }
    started_ = false;
  }

  [[nodiscard]] auto port() const noexcept -> int {
    return port_;
  }

private:
  auto run() -> void {
    sockaddr_in peer{};
    socklen_t peer_len = sizeof(peer);
    int cfd = ::accept(fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (cfd < 0) {
      return;
    }

    if (behavior_ == behavior::close_immediately) {
      ::shutdown(cfd, SHUT_RDWR);
      ::close(cfd);
      return;
    }

    if (behavior_ == behavior::hang_after_accept) {
      // Read a little (optional), then hang without responding.
      char buf[256];
      (void)::recv(cfd, buf, sizeof(buf), 0);
      while (!stop_requested_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
      }
      ::shutdown(cfd, SHUT_RDWR);
      ::close(cfd);
      return;
    }
  }

  behavior behavior_;
  int fd_{-1};
  int port_{0};
  std::atomic<bool> stop_requested_{false};
  std::thread th_{};
  bool started_{false};
};

}  // namespace

TEST(redis_integration, ping_set_get) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = 6379;
  cfg.connect_timeout = 300ms;
  cfg.request_timeout = 500ms;
  cfg.reconnection.enabled = false;  // integration test should fail fast when Redis is absent

  bool skipped = false;
  std::string skip_reason{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    rediscoro::client c{ctx.get_executor(), cfg};

    auto ec = co_await c.connect();
    if (ec) {
      skipped = true;
      skip_reason = "Redis not reachable at 127.0.0.1:6379 (connect failed: " + ec.message() + ")";
      guard.reset();
      co_return;
    }

    auto pong = co_await c.exec<std::string>("PING");
    if (!pong.get<0>().has_value()) {
      diag = "PING failed";
      guard.reset();
      co_return;
    }
    if (*pong.get<0>() != "PONG") {
      diag = "PING unexpected reply: " + *pong.get<0>();
      guard.reset();
      co_return;
    }

    auto setr = co_await c.exec<std::string>("SET", "rediscoro_test_key", "v1");
    if (!setr.get<0>().has_value()) {
      diag = "SET failed";
      guard.reset();
      co_return;
    }
    if (*setr.get<0>() != "OK") {
      diag = "SET unexpected reply: " + *setr.get<0>();
      guard.reset();
      co_return;
    }

    auto getr = co_await c.exec<std::string>("GET", "rediscoro_test_key");
    if (!getr.get<0>().has_value()) {
      diag = "GET failed";
      guard.reset();
      co_return;
    }
    if (*getr.get<0>() != "v1") {
      diag = "GET unexpected reply: " + *getr.get<0>();
      guard.reset();
      co_return;
    }

    ok = true;

    co_await c.close();
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

TEST(client_integration, exec_without_connect_is_rejected) {
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

TEST(client_integration, connect_handshake_timeout) {
  local_tcp_server srv{local_tcp_server::behavior::hang_after_accept};
  ASSERT_TRUE(srv.start());

  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    rediscoro::config cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = srv.port();
    cfg.connect_timeout = 50ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto ec = co_await c.connect();
    if (ec != rediscoro::error::handshake_timeout) {
      diag = "expected handshake_timeout, got: " + ec.message();
      guard.reset();
      co_return;
    }

    ok = true;
    guard.reset();
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  srv.stop();
  ASSERT_TRUE(ok) << diag;
}

TEST(client_integration, connect_peer_close_during_handshake) {
  local_tcp_server srv{local_tcp_server::behavior::close_immediately};
  ASSERT_TRUE(srv.start());

  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    rediscoro::config cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = srv.port();
    cfg.connect_timeout = 200ms;
    cfg.reconnection.enabled = false;

    rediscoro::client c{ctx.get_executor(), cfg};
    auto ec = co_await c.connect();
    if (ec != rediscoro::error::connection_reset) {
      diag = "expected connection_reset, got: " + ec.message();
      guard.reset();
      co_return;
    }

    ok = true;
    guard.reset();
    co_return;
  };

  iocoro::co_spawn(ctx.get_executor(), task(), iocoro::detached);
  ctx.run();

  srv.stop();
  ASSERT_TRUE(ok) << diag;
}

TEST(redis_integration, request_timeout_triggers_reconnect) {
  iocoro::io_context ctx;
  auto guard = iocoro::make_work_guard(ctx);

  rediscoro::config cfg{};
  cfg.host = "127.0.0.1";
  cfg.port = 6379;
  cfg.connect_timeout = 300ms;
  cfg.request_timeout = 50ms;
  cfg.reconnection.enabled = true;
  cfg.reconnection.immediate_attempts = 3;
  cfg.reconnection.initial_delay = 50ms;
  cfg.reconnection.max_delay = 200ms;

  bool skipped = false;
  std::string skip_reason{};
  bool ok = false;
  std::string diag{};

  auto task = [&]() -> iocoro::awaitable<void> {
    rediscoro::client c{ctx.get_executor(), cfg};

    auto ec = co_await c.connect();
    if (ec) {
      skipped = true;
      skip_reason = "Redis not reachable at 127.0.0.1:6379 (connect failed: " + ec.message() + ")";
      guard.reset();
      co_return;
    }

    // Force a long-running request so request_timeout triggers.
    // BLPOP on a missing key blocks on the server; our client should time out first.
    rediscoro::request blpop{};
    blpop.push("BLPOP", "rediscoro_timeout_key", "10");
    auto r = co_await c.exec_dynamic<rediscoro::ignore_t>(std::move(blpop));
    if (r.size() != 1) {
      diag = "unexpected response size from BLPOP";
      guard.reset();
      co_return;
    }
    if (r[0].has_value()) {
      diag = "expected request_timeout, got value";
      guard.reset();
      co_return;
    }
    if (!r[0].error().is_client_error()) {
      diag = "expected client error from timeout";
      guard.reset();
      co_return;
    }
    if (r[0].error().as_client_error() != rediscoro::error::request_timeout) {
      diag = "expected request_timeout";
      guard.reset();
      co_return;
    }

    // Wait for automatic reconnection to restore OPEN, then verify PING works again.
    bool ping_ok = false;
    for (int i = 0; i < 40; ++i) {
      auto pong = co_await c.exec<std::string>("PING");
      if (pong.get<0>().has_value() && *pong.get<0>() == "PONG") {
        ping_ok = true;
        break;
      }
      co_await co_sleep(ctx.get_executor(), 50ms);
    }
    if (!ping_ok) {
      diag = "PING did not recover after request_timeout";
      guard.reset();
      co_return;
    }

    ok = true;
    co_await c.close();
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


