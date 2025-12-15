#include <xz/io/error.hpp>
#include <xz/io/tcp_socket.hpp>
#include <xz/redis/detail/assert.hpp>
#include <xz/redis/detail/connection.hpp>
#include <xz/redis/detail/pipeline.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/resp3/node.hpp>

namespace xz::redis::detail {

connection::connection(io::io_context& ctx, config cfg) : ctx_{ctx}, cfg_{std::move(cfg)}, socket_{ctx_}, parser_{} {}

connection::~connection() { stop(); }

auto connection::ensure_pipeline() -> void {
  if (pipeline_ && !pipeline_->stopped()) return;

  // Pipeline is an internal implementation detail: users call connection.execute().
  pipeline_ = std::make_unique<pipeline>(
      ctx_, [this](request const& req) -> io::awaitable<void> { co_await this->async_write(req); },
      [this](std::error_code ec) { this->fail(ec); }, cfg_.request_timeout);
}

auto connection::run() -> io::awaitable<void> {
  // By design: run() is only valid from stable "not connected" states.
  if (!is_inactive_state()) {
    throw std::system_error(io::error::operation_failed);
  }

  // Starting/restarting a connection attempt.
  state_ = state::connecting;

  parser_.reset();
  error_ = {};

  try {
    // TCP connect
    auto endpoint = io::ip::tcp_endpoint{io::ip::address_v4::from_string(cfg_.host), cfg_.port};
    co_await socket_.async_connect(endpoint, cfg_.connect_timeout);

    state_ = state::running;

    // Start read loop AFTER successful connect
    io::co_spawn(ctx_, read_loop(), io::use_detached);
  } catch (std::system_error const& e) {
    // Connection attempt failed.
    fail(e.code());
    throw;
  } catch (...) {
    // Normalize unknown failures into an error_code-based exception.
    fail(io::error::operation_failed);
    throw std::system_error(error_);
  }
}

auto connection::execute_any(request const& req, adapter::any_adapter adapter) -> io::awaitable<void> {
  if (state_ != state::running) {
    throw std::system_error(io::error::not_connected);
  }
  ensure_pipeline();
  co_await pipeline_->execute_any(req, std::move(adapter));
}

auto connection::read_loop() -> io::awaitable<void> {
  auto gen = parser_.parse();

  try {
    for (;;) {
      auto span = parser_.prepare(4096);
      auto n = co_await socket_.async_read_some(std::span<char>{span.data(), span.size()}, {});

      parser_.commit(n);

      // Process all complete messages in buffer
      while (gen.next()) {
        auto& msg_opt = gen.value();
        if (!msg_opt) {
          // Parser needs more data
          break;
        }
        if (pipeline_) {
          pipeline_->on_msg(*msg_opt);
        }
      }

      // Check for parser error
      if (auto ec = parser_.error()) {
        fail(ec);
        co_return;
      }
    }
  } catch (std::system_error const& e) {
    // Detached read loop: translate termination into connection error state.
    auto ec = e.code();

    // Expected shutdown paths:
    // - user called stop() (state::stopped -> close -> abort)
    // - connection already transitioned to failed and closed transport
    if (ec == io::error::operation_aborted || ec == io::error::not_connected) {
      // If we're no longer running, an abort/not_connected is expected during teardown.
      if (state_ != state::running) {
        co_return;
      }
    }

    fail(ec);
    co_return;
  } catch (...) {
    fail(io::error::operation_failed);
    co_return;
  }
}

auto connection::async_write(request const& req) -> io::awaitable<void> {
  if (state_ != state::running) {
    throw std::system_error(io::error::not_connected);
  }

  auto payload = req.payload();
  co_await io::async_write(socket_, std::span<char const>{payload.data(), payload.size()}, cfg_.request_timeout);
}

void connection::fail(std::error_code ec) {
  if (is_inactive_state()) {
    return;
  }

  state_ = state::failed;
  error_ = ec;

  if (pipeline_) {
    pipeline_->on_error(ec);
  }

  // TODO: Add logging here if needed
  // logger_.error("Connection failed: {}", ec.message());

  close_transport();
}

void connection::stop() {
  if (is_inactive_state()) {
    return;
  }

  state_ = state::stopped;

  if (pipeline_ && !pipeline_->stopped()) {
    pipeline_->on_close();
  }

  close_transport();
}

auto connection::error() const -> std::error_code { return error_; }

void connection::close_transport() noexcept {
  try {
    socket_.close();
  } catch (...) {
  }
  parser_.reset();
}

}  // namespace xz::redis::detail
