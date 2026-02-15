#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rediscoro::test_support {

class fake_redis_server {
 public:
  struct action {
    enum class kind {
      read,
      write,
      sleep,
      close,
    };

    kind op{kind::read};
    std::string payload{};
    std::size_t min_bytes{1};
    std::chrono::milliseconds timeout{1000};
    std::chrono::milliseconds delay{0};

    [[nodiscard]] static auto read(std::size_t min = 1,
                                   std::chrono::milliseconds timeout = std::chrono::milliseconds{
                                     1000}) -> action {
      action a{};
      a.op = kind::read;
      a.min_bytes = min;
      a.timeout = timeout;
      return a;
    }

    [[nodiscard]] static auto write(std::string data) -> action {
      action a{};
      a.op = kind::write;
      a.payload = std::move(data);
      return a;
    }

    [[nodiscard]] static auto sleep_for(std::chrono::milliseconds d) -> action {
      action a{};
      a.op = kind::sleep;
      a.delay = d;
      return a;
    }

    [[nodiscard]] static auto close_client() -> action {
      action a{};
      a.op = kind::close;
      return a;
    }
  };

  using session_script = std::vector<action>;

  explicit fake_redis_server(std::vector<session_script> sessions)
      : sessions_(std::move(sessions)), session_reads_(sessions_.size()) {
    thread_ = std::thread([this] { run(); });

    std::unique_lock lock(mu_);
    if (!cv_.wait_for(lock, std::chrono::seconds(2), [this] { return ready_; })) {
      stop_ = true;
      lock.unlock();
      stop();
      throw std::runtime_error("fake_redis_server startup timeout");
    }

    if (!failure_.empty()) {
      const auto msg = failure_;
      lock.unlock();
      stop();
      throw std::runtime_error(msg);
    }
  }

  fake_redis_server(fake_redis_server const&) = delete;
  auto operator=(fake_redis_server const&) -> fake_redis_server& = delete;

  ~fake_redis_server() { stop(); }

  auto stop() -> void {
    const bool already = stop_.exchange(true, std::memory_order_relaxed);
    if (already) {
      if (thread_.joinable()) {
        thread_.join();
      }
      return;
    }

    close_listen_fd();
    close_active_client_fd();

    if (thread_.joinable()) {
      thread_.join();
    }
  }

  [[nodiscard]] auto host() const -> const char* { return "127.0.0.1"; }

  [[nodiscard]] auto port() const -> std::uint16_t {
    std::lock_guard lock(mu_);
    return port_;
  }

  [[nodiscard]] auto accepted_count() const -> std::size_t {
    return accepted_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] auto session_read(std::size_t idx) const -> std::string {
    std::lock_guard lock(mu_);
    if (idx >= session_reads_.size()) {
      return {};
    }
    return session_reads_[idx];
  }

  [[nodiscard]] auto failure_message() const -> std::string {
    std::lock_guard lock(mu_);
    return failure_;
  }

 private:
  auto run() -> void {
    auto fail = [this](std::string msg) {
      std::lock_guard lock(mu_);
      failure_ = std::move(msg);
      ready_ = true;
      cv_.notify_all();
    };

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
      fail("fake_redis_server: socket() failed");
      return;
    }

    {
      std::lock_guard lock(mu_);
      listen_fd_ = listen_fd;
    }

    int one = 1;
    (void)::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(0);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      fail("fake_redis_server: bind() failed");
      close_listen_fd();
      return;
    }

    if (::listen(listen_fd, 16) != 0) {
      fail("fake_redis_server: listen() failed");
      close_listen_fd();
      return;
    }

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
      fail("fake_redis_server: getsockname() failed");
      close_listen_fd();
      return;
    }

    {
      std::lock_guard lock(mu_);
      port_ = ntohs(bound.sin_port);
      ready_ = true;
      cv_.notify_all();
    }

    for (std::size_t i = 0; i < sessions_.size(); ++i) {
      if (stop_.load(std::memory_order_relaxed)) {
        break;
      }

      int client_fd = accept_one(listen_fd);
      if (client_fd < 0) {
        if (stop_.load(std::memory_order_relaxed)) {
          break;
        }
        fail("fake_redis_server: accept() failed");
        break;
      }

      accepted_count_.fetch_add(1, std::memory_order_relaxed);
      run_session(i, client_fd);
      if (client_fd >= 0) {
        (void)::close(client_fd);
      }
    }

    close_listen_fd();
  }

  [[nodiscard]] auto accept_one(int listen_fd) const -> int {
    for (;;) {
      if (stop_.load(std::memory_order_relaxed)) {
        return -1;
      }

      sockaddr_in peer{};
      socklen_t len = sizeof(peer);
      int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&peer), &len);
      if (fd >= 0) {
        return fd;
      }

      if (errno == EINTR) {
        continue;
      }

      return -1;
    }
  }

  auto run_session(std::size_t idx, int& client_fd) -> void {
    {
      std::lock_guard lock(mu_);
      active_client_fd_ = client_fd;
    }

    std::string captured{};

    for (const auto& act : sessions_[idx]) {
      if (stop_.load(std::memory_order_relaxed)) {
        break;
      }

      if (client_fd < 0) {
        break;
      }

      switch (act.op) {
        case action::kind::read: {
          captured += recv_min_bytes(client_fd, act.min_bytes, act.timeout);
          break;
        }
        case action::kind::write: {
          (void)send_all(client_fd, act.payload);
          break;
        }
        case action::kind::sleep: {
          std::this_thread::sleep_for(act.delay);
          break;
        }
        case action::kind::close: {
          (void)::close(client_fd);
          client_fd = -1;
          break;
        }
      }
    }

    std::lock_guard lock(mu_);
    session_reads_[idx] = std::move(captured);
    active_client_fd_ = -1;
  }

  [[nodiscard]] static auto recv_min_bytes(int fd, std::size_t min_bytes,
                                           std::chrono::milliseconds timeout) -> std::string {
    std::string out{};
    out.reserve(min_bytes);

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (out.size() < min_bytes) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        break;
      }

      const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      timeval tv{};
      tv.tv_sec = static_cast<long>(remain.count() / 1000);
      tv.tv_usec = static_cast<long>((remain.count() % 1000) * 1000);

      fd_set rfds;
      FD_ZERO(&rfds);
      FD_SET(fd, &rfds);

      const int ready = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
      if (ready <= 0) {
        break;
      }

      char buf[4096];
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        break;
      }
      out.append(buf, static_cast<std::size_t>(n));
    }

    return out;
  }

  [[nodiscard]] static auto send_all(int fd, std::string const& data) -> bool {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const auto* ptr = data.data() + sent;
      const auto len = data.size() - sent;

#ifdef MSG_NOSIGNAL
      const ssize_t n = ::send(fd, ptr, len, MSG_NOSIGNAL);
#else
      const ssize_t n = ::send(fd, ptr, len, 0);
#endif

      if (n <= 0) {
        return false;
      }

      sent += static_cast<std::size_t>(n);
    }

    return true;
  }

  auto close_listen_fd() -> void {
    int fd = -1;
    {
      std::lock_guard lock(mu_);
      fd = listen_fd_;
      listen_fd_ = -1;
    }

    if (fd >= 0) {
      (void)::shutdown(fd, SHUT_RDWR);
      (void)::close(fd);
    }
  }

  auto close_active_client_fd() -> void {
    int fd = -1;
    {
      std::lock_guard lock(mu_);
      fd = active_client_fd_;
      active_client_fd_ = -1;
    }
    if (fd >= 0) {
      (void)::shutdown(fd, SHUT_RDWR);
      (void)::close(fd);
    }
  }

 private:
  std::vector<session_script> sessions_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  bool ready_{false};
  std::uint16_t port_{0};
  int listen_fd_{-1};
  int active_client_fd_{-1};
  std::string failure_{};
  std::vector<std::string> session_reads_{};

  std::atomic<bool> stop_{false};
  std::atomic<std::size_t> accepted_count_{0};
  std::thread thread_{};
};

}  // namespace rediscoro::test_support
