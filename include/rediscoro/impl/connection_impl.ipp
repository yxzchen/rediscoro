#pragma once

#include <iocoro/co_sleep.hpp>
#include <iocoro/co_spawn.hpp>
#include <iocoro/error.hpp>
#include <iocoro/io/async_write.hpp>
#include <iocoro/io/with_timeout.hpp>
#include <iocoro/ip/address.hpp>
#include <iocoro/ip/tcp.hpp>
#include <iocoro/when_all.hpp>
#include <rediscoro/adapter/result.hpp>
#include <rediscoro/detail/assert.hpp>
#include <rediscoro/detail/connection_impl.hpp>
#include <rediscoro/detail/pipeline.hpp>
#include <rediscoro/error.hpp>
#include <rediscoro/ignore.hpp>
#include <rediscoro/resp3/node.hpp>
#include <rediscoro/response.hpp>

#include <cerrno>
#include <cstddef>
#include <span>
#include <system_error>

namespace rediscoro::detail {

connection_impl::connection_impl(iocoro::executor ex, config cfg)
    : cfg_{std::move(cfg)}, ex_{ex}, socket_{ex_}, parser_{} {}

connection_impl::~connection_impl() { stop(); }

auto connection_impl::ensure_pipeline() -> void {
  if (pipeline_ && !pipeline_->stopped()) {
    return;
  }

  pipeline_config pcfg{
      .ex = ex_,
      .write_fn = [this](request const& req) -> iocoro::awaitable<void> { co_await this->async_write(req); },
      .error_fn = [this](std::error_code ec) { this->fail(ec); },
      .request_timeout = cfg_.request_timeout,
      .max_inflight = 0,
  };
  pipeline_ = std::make_shared<pipeline>(std::move(pcfg));
}

auto connection_impl::run() -> iocoro::awaitable<void> {
  if (!is_inactive_state()) {
    throw std::system_error(std::make_error_code(std::errc::operation_not_permitted));
  }

  state_ = state::connecting;
  error_ = {};
  parser_.reset();

  try {
    auto addr_r = iocoro::ip::address::from_string(cfg_.host);
    if (!addr_r) {
      throw std::system_error(addr_r.error());
    }
    auto endpoint = iocoro::ip::tcp::endpoint{*addr_r, cfg_.port};

    auto r = co_await iocoro::io::with_timeout(socket_, socket_.async_connect(endpoint), cfg_.connect_timeout);
    if (r) {
      throw std::system_error(r);
    }

    // Connected. Mark running before starting read_loop/handshake so internal pipeline executes are allowed.
    // (run() doesn't complete until handshake is done, so callers still can't issue requests early.)
    state_ = state::running;

    // Pipeline is used for handshake and all subsequent requests.
    ensure_pipeline();

    // Spawn read_loop with shared_from_this() - keeps impl alive
    auto self = shared_from_this();
    read_task_ = iocoro::co_spawn(ex_, [self]() { return self->read_loop(); }, iocoro::use_awaitable);

    co_await handshake();
  } catch (std::system_error const& e) {
    // Connection attempt failed.
    fail(e.code());
    throw;
  } catch (...) {
    // Normalize unknown failures into an error_code-based exception.
    fail(std::make_error_code(std::errc::io_error));
    throw std::system_error(error_);
  }
}

auto connection_impl::execute_any(request const& req, adapter::any_adapter adapter) -> iocoro::awaitable<void> {
  if (state_ != state::running) {
    throw std::system_error(iocoro::error::not_connected);
  }
  ensure_pipeline();
  co_await pipeline_->execute_any(req, std::move(adapter));
}

auto connection_impl::read_loop() -> iocoro::awaitable<void> {
  auto gen = parser_.parse();

  for (;;) {
    auto span = parser_.prepare(4096);
    auto buf = std::as_writable_bytes(std::span<char>{span.data(), span.size()});
    auto r = co_await socket_.async_read_some(buf);
    if (!r) {
      auto ec = r.error();

      // treat abort/not_connected as teardown signals and just exit.
      // The lifecycle layer (run()/handshake()/write/pipeline) is responsible for reporting failures via fail().
      if (ec == iocoro::error::operation_aborted || ec == iocoro::error::not_connected) {
        co_return;
      }
      fail(ec);
      co_return;
    }

    auto const n = *r;
    if (n == 0) {
      fail(iocoro::error::eof);
      co_return;
    }

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
}

auto connection_impl::async_write(request const& req) -> iocoro::awaitable<void> {
  if (state_ != state::running) {
    throw std::system_error(iocoro::error::not_connected);
  }

  auto payload = req.payload();
  auto bytes = std::as_bytes(std::span<char const>{payload.data(), payload.size()});
  auto r = co_await iocoro::io::async_write_timeout(socket_, bytes, cfg_.request_timeout);
  if (!r) {
    throw std::system_error(r.error());
  }
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
    reconnect_task_ = iocoro::co_spawn(ex_, [self]() { return self->reconnect_loop(); }, iocoro::use_awaitable);
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

auto connection_impl::graceful_stop() -> iocoro::awaitable<void> {
  stop();

  // Wait for background tasks to complete
  std::vector<iocoro::awaitable<void>> tasks;
  if (read_task_.has_value()) {
    tasks.push_back(std::move(*read_task_));
    read_task_.reset();
  }
  if (reconnect_task_.has_value()) {
    tasks.push_back(std::move(*reconnect_task_));
    reconnect_task_.reset();
  }

  if (!tasks.empty()) {
    (void)co_await iocoro::when_all(std::move(tasks));
  }
}

auto connection_impl::current_state() const noexcept -> state { return state_; }

auto connection_impl::is_running() const noexcept -> bool { return state_ == state::running; }

auto connection_impl::error() const -> std::error_code { return error_; }

auto connection_impl::get_executor() noexcept -> iocoro::executor { return ex_; }

void connection_impl::close_transport() noexcept {
  socket_.close();
  parser_.reset();
}

auto connection_impl::reconnect_loop() -> iocoro::awaitable<void> {
  while (reconnect_active_) {
    if (cfg_.reconnect_delay.count() > 0) {
      co_await iocoro::co_sleep(cfg_.reconnect_delay);
    }

    if (!reconnect_active_ || state_ == state::stopped) {
      reconnect_active_ = false;
      co_return;
    }

    try {
      co_await run();
      reconnect_active_ = false;
      co_return;
    } catch (...) {
      if (!reconnect_active_ || state_ == state::stopped) {
        reconnect_active_ = false;
        co_return;
      }
    }
  }
}

auto connection_impl::handshake() -> iocoro::awaitable<void> {
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
    throw std::system_error(std::make_error_code(std::errc::io_error));
  }

  for (std::size_t i = 0; i < resp.size(); ++i) {
    if (!resp[i].has_value()) {
      auto const& op = (i < ops.size() ? ops[i] : std::string("HANDSHAKE"));
      throw std::system_error(make_error_code(rediscoro::error::resp3_simple_error),
                              op + ": " + resp[i].error().message);
    }
  }
}

}  // namespace rediscoro::detail
