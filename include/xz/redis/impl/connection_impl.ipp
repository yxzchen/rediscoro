#pragma once

#include <xz/io/co_sleep.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/io/error.hpp>
#include <xz/io/tcp_socket.hpp>
#include <xz/io/when_all.hpp>
#include <xz/redis/adapter/result.hpp>
#include <xz/redis/detail/assert.hpp>
#include <xz/redis/detail/connection_impl.hpp>
#include <xz/redis/detail/pipeline.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/ignore.hpp>
#include <xz/redis/resp3/node.hpp>
#include <xz/redis/response.hpp>

namespace xz::redis::detail {

connection_impl::connection_impl(io::io_context& ctx, config cfg)
    : ctx_{ctx}, cfg_{std::move(cfg)}, socket_{ctx_}, parser_{} {}

connection_impl::~connection_impl() { stop(); }

auto connection_impl::ensure_pipeline() -> void {
  if (pipeline_ && !pipeline_->stopped()) return;

  pipeline_config pcfg{
      .ex = ctx_,
      .write_fn = [this](request const& req) -> io::awaitable<void> { co_await this->async_write(req); },
      .error_fn = [this](std::error_code ec) { this->fail(ec); },
      .request_timeout = cfg_.request_timeout,
      .max_inflight = 0,
  };
  pipeline_ = std::make_shared<pipeline>(std::move(pcfg));
}

auto connection_impl::run() -> io::awaitable<void> {
  if (!is_inactive_state()) {
    throw std::system_error(io::error::operation_failed);
  }

  state_ = state::connecting;
  error_ = {};
  parser_.reset();

  try {
    auto endpoint = io::ip::tcp_endpoint{io::ip::address_v4::from_string(cfg_.host), cfg_.port};
    co_await socket_.async_connect(endpoint, cfg_.connect_timeout);

    // Connected. Mark running before starting read_loop/handshake so internal pipeline executes are allowed.
    // (run() doesn't complete until handshake is done, so callers still can't issue requests early.)
    state_ = state::running;

    // Pipeline is used for handshake and all subsequent requests.
    ensure_pipeline();

    // Spawn read_loop with shared_from_this() - keeps impl alive
    // Note: use_detached here because read_loop runs for the entire connection lifetime
    // and doesn't need to be awaited (graceful_stop will close socket to terminate it)
    auto self = shared_from_this();
    io::co_spawn(ctx_, [self]() { return self->read_loop(); }, io::use_detached);

    co_await handshake();
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

auto connection_impl::execute_any(request const& req, adapter::any_adapter adapter) -> io::awaitable<void> {
  if (state_ != state::running) {
    throw std::system_error(io::error::not_connected);
  }
  ensure_pipeline();
  co_await pipeline_->execute_any(req, std::move(adapter));
}

auto connection_impl::read_loop() -> io::awaitable<void> {
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
    auto ec = e.code();

    // treat abort/not_connected as teardown signals and just exit.
    // The lifecycle layer (run()/handshake()/write/pipeline) is responsible for reporting failures via fail().
    if (ec == io::error::operation_aborted || ec == io::error::not_connected) {
      co_return;
    }
    fail(ec);
    co_return;
  } catch (...) {
    fail(io::error::operation_failed);
    co_return;
  }
}

auto connection_impl::async_write(request const& req) -> io::awaitable<void> {
  if (state_ != state::running) {
    throw std::system_error(io::error::not_connected);
  }

  auto payload = req.payload();
  co_await io::async_write(socket_, std::span<char const>{payload.data(), payload.size()}, cfg_.request_timeout);
}

void connection_impl::fail(std::error_code ec) {
  if (is_inactive_state()) {
    return;
  }

  state_ = state::failed;
  error_ = ec;

  if (pipeline_) {
    pipeline_->stop(ec);
  }

  close_transport();

  // Start auto-reconnect loop if enabled and not already running
  if (cfg_.auto_reconnect && !reconnect_active_) {
    reconnect_active_ = true;
    auto self = shared_from_this();
    reconnect_task_ = io::co_spawn(ctx_, [self]() { return self->reconnect_loop(); }, io::use_awaitable);
  }
}

void connection_impl::stop() {
  reconnect_active_ = false;

  if (is_inactive_state()) {
    return;
  }

  state_ = state::stopped;

  if (pipeline_ && !pipeline_->stopped()) {
    pipeline_->stop();
  }

  close_transport();
}

auto connection_impl::graceful_stop() -> io::awaitable<void> {
  stop();

  // Wait for background tasks to complete
  // Note: read_loop uses use_detached so can't be awaited; socket close will terminate it
  if (reconnect_active_ && reconnect_task_.has_value()) {
    co_await std::move(*reconnect_task_);
    reconnect_task_.reset();
  }
}

auto connection_impl::current_state() const noexcept -> state { return state_; }

auto connection_impl::is_running() const noexcept -> bool { return state_ == state::running; }

auto connection_impl::error() const -> std::error_code { return error_; }

auto connection_impl::get_executor() noexcept -> io::io_context& { return ctx_; }

void connection_impl::close_transport() noexcept {
  try {
    socket_.close();
  } catch (...) {
  }
  parser_.reset();
}

auto connection_impl::reconnect_loop() -> io::awaitable<void> {
  while (reconnect_active_ && cfg_.auto_reconnect) {
    if (cfg_.reconnect_delay.count() > 0) {
      co_await io::co_sleep(ctx_, cfg_.reconnect_delay);
    }

    if (!reconnect_active_ || state_ == state::stopped) {
      break;
    }

    try {
      co_await run();
      break;  // Success
    } catch (...) {
      if (!reconnect_active_ || state_ == state::stopped) {
        break;
      }
      // Continue loop on reconnect failure
    }
  }
  reconnect_active_ = false;
  reconnect_task_.reset();
}

auto connection_impl::handshake() -> io::awaitable<void> {
  request req;
  req.reserve(256);
  std::vector<std::string> ops;
  ops.reserve(4);

  // Build a single multi-command request; replies will be FIFO and parsed into `dynamic_response`.

  // 1) AUTH (server-driven errors)
  // If either username/password is non-empty, send AUTH and let Redis respond with OK/ERR.
  if (!cfg_.username.empty() || !cfg_.password.empty()) {
    if (!cfg_.username.empty()) {
      req.push("AUTH", cfg_.username, cfg_.password);
    } else {
      req.push("AUTH", cfg_.password);
    }
    ops.emplace_back("AUTH");
  }

  // 2) HELLO <2|3>
  {
    auto const proto = (cfg_.version == resp_version::resp3) ? 3 : 2;
    req.push("HELLO", proto);
    ops.emplace_back("HELLO");
  }

  // 3) CLIENT SETNAME <name>
  if (!cfg_.client_name.empty()) {
    req.push("CLIENT", "SETNAME", cfg_.client_name);
    ops.emplace_back("CLIENT SETNAME");
  }

  // 4) SELECT <db>
  if (cfg_.database != 0) {
    req.push("SELECT", cfg_.database);
    ops.emplace_back("SELECT");
  }

  dynamic_response<ignore_t> resp;
  co_await execute(req, resp);

  if (resp.size() != req.expected_responses()) {
    throw std::system_error(io::error::operation_failed);
  }

  for (std::size_t i = 0; i < resp.size(); ++i) {
    if (!resp[i].has_value()) {
      auto const& op = (i < ops.size() ? ops[i] : std::string("HANDSHAKE"));
      throw std::system_error(make_error_code(xz::redis::error::resp3_simple_error),
                              op + ": " + resp[i].error().message);
    }
  }
}

}  // namespace xz::redis::detail
