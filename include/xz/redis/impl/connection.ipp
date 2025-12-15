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

  // Mark running before we suspend on connect to prevent concurrent run() calls
  // from spawning multiple connect/read loops.
  running_ = true;

  parser_.reset();
  error_ = {};

  try {
    // TCP connect
    auto endpoint = io::ip::tcp_endpoint{io::ip::address_v4::from_string(cfg_.host), cfg_.port};
    co_await socket_.async_connect(endpoint, cfg_.connect_timeout);

    // Start read loop AFTER successful connect
    io::co_spawn(ctx_, read_loop(), io::use_detached);
  } catch (std::system_error const& e) {
    // Ensure the connection can be retried and no resources remain open.
    error_ = e.code();
    running_ = false;
    socket_.close();
    throw;
  } catch (...) {
    // Normalize unknown failures into an error_code-based exception.
    error_ = io::error::operation_failed;
    running_ = false;
    socket_.close();
    throw std::system_error(error_);
  }
}

auto connection::read_loop() -> io::awaitable<void> {
  auto gen = parser_.parse();

  try {
    for (;;) {
      auto span = parser_.prepare(4096);
      auto n = co_await socket_.async_read_some(std::span<char>{span.data(), span.size()}, {});

      // In ioxz, EOF is surfaced as an exception (std::system_error(error::eof)),
      // so n==0 should not be observable here.
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
  } catch (std::system_error const& e) {
    // Detached read loop: translate termination into connection error state.
    //
    // - If stop() already ran, running_ is false and fail() is a no-op.
    // - If the socket was closed/cancelled, treat as a clean stop.
    if (e.code() != io::error::operation_aborted) {
      fail(e.code());
    } else {
      stop();
    }
    co_return;
  } catch (...) {
    fail(io::error::operation_failed);
    co_return;
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

auto connection::error() const -> std::error_code {
  return error_;
}

}  // namespace xz::redis::detail
