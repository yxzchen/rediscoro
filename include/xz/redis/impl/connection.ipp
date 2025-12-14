#include <xz/io/error.hpp>
#include <xz/redis/detail/connection.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/resp3/node.hpp>

namespace xz::redis::detail {

connection::connection(io::io_context& ctx, config cfg)
    : ctx_{ctx},
      cfg_{std::move(cfg)},
      socket_{ctx_},
      parser_{} {}

connection::~connection() {
  stop();
}

auto connection::run() -> io::awaitable<void> {
  if (running_) {
    co_return;
  }

  parser_.reset();
  error_ = {};

  // TCP connect
  auto endpoint = io::ip::tcp_endpoint{io::ip::address_v4::from_string(cfg_.host), cfg_.port};
  co_await socket_.async_connect(endpoint, cfg_.connect_timeout);

  running_ = true;

  // Start read loop AFTER successful connect
  io::co_spawn(ctx_, read_loop(), io::use_detached);
}

auto connection::read_loop() -> io::awaitable<void> {
  auto gen = parser_.parse();

  for (;;) {
    auto span = parser_.prepare(4096);
    auto n = co_await socket_.async_read_some(std::span<char>{span.data(), span.size()}, {});

    if (n == 0) {
      // EOF
      fail(io::error::eof);
      co_return;
    }

    parser_.commit(n);

    // Process all complete messages in buffer
    while (gen.next()) {
      auto msg_opt = gen.value();
      if (!msg_opt) {
        // Parser needs more data
        break;
      }
      // Messages will be handled by pipeline (to be implemented)
    }

    // Check for parser error
    if (auto ec = parser_.error()) {
      fail(ec);
      co_return;
    }
  }
}

void connection::fail(std::error_code ec) {
  if (!running_) {
    return;
  }

  error_ = ec;

  // TODO: Add logging here if needed
  // logger_.error("Connection failed: {}", ec.message());

  // Clean up resources
  stop();
}

void connection::stop() {
  if (running_) {
    running_ = false;
    socket_.close();
    parser_.reset();
  }
}

auto connection::is_running() const -> bool {
  return running_;
}

}  // namespace xz::redis::detail
