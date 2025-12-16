#include <xz/io/error.hpp>
#include <xz/io/tcp_socket.hpp>
#include <xz/io/when_all.hpp>
#include <xz/redis/detail/assert.hpp>
#include <xz/redis/connection.hpp>
#include <xz/redis/detail/pipeline.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/adapter/result.hpp>
#include <xz/redis/response.hpp>
#include <xz/redis/ignore.hpp>
#include <xz/redis/resp3/node.hpp>

namespace xz::redis {

connection::connection(io::io_context& ctx, config cfg) : ctx_{ctx}, cfg_{std::move(cfg)}, socket_{ctx_}, parser_{} {}

connection::~connection() { stop(); }

auto connection::ensure_pipeline() -> void {
  if (pipeline_ && !pipeline_->stopped()) return;

  // Pipeline is an internal implementation detail: users call connection.execute().
  detail::pipeline_config pcfg{
      .ex = ctx_,
      .write_fn = [this](request const& req) -> io::awaitable<void> { co_await this->async_write(req); },
      .error_fn = [this](std::error_code ec) { this->fail(ec); },
      .request_timeout = cfg_.request_timeout,
      .max_inflight = 0 /* 0 = unlimited */,
  };
  pipeline_ = std::make_shared<detail::pipeline>(std::move(pcfg));
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

    // Pipeline is used for handshake and all subsequent requests.
    ensure_pipeline();

    // Start read loop AFTER successful connect
    // Use a lambda factory to avoid immediate invocation and ensure proper lifetime management
    io::co_spawn(ctx_, [this]() { return read_loop(); }, io::use_detached);

    // Handshake (HELLO/AUTH/SELECT/CLIENT SETNAME).
    // Build multiple single-command requests, start them all, then validate each response.
    {
      std::vector<request> reqs;
      std::vector<adapter::result<ignore_t>> resps;
      std::vector<std::string> ops;
      reqs.reserve(4);
      resps.reserve(4);
      ops.reserve(4);

      // 1) HELLO <2|3>
      {
        auto const proto = (cfg_.version == resp_version::resp3) ? "3" : "2";
        reqs.emplace_back();
        reqs.back().reserve(128);
        reqs.back().push("HELLO", proto);
        resps.emplace_back();
        ops.emplace_back("HELLO");
      }

      // 2) AUTH (if either username/password is specified)
      if (cfg_.username.has_value() || cfg_.password.has_value()) {
        if (!cfg_.password.has_value()) {
          throw std::system_error(io::error::operation_failed);
        }
        reqs.emplace_back();
        reqs.back().reserve(128);
        if (cfg_.username.has_value()) {
          reqs.back().push("AUTH", *cfg_.username, *cfg_.password);
        } else {
          reqs.back().push("AUTH", *cfg_.password);
        }
        resps.emplace_back();
        ops.emplace_back("AUTH");
      }

      // 3) SELECT <db>
      if (cfg_.database != 0) {
        reqs.emplace_back();
        reqs.back().reserve(128);
        reqs.back().push("SELECT", cfg_.database);
        resps.emplace_back();
        ops.emplace_back("SELECT");
      }

      // 4) CLIENT SETNAME <name>
      if (cfg_.client_name.has_value()) {
        reqs.emplace_back();
        reqs.back().reserve(128);
        reqs.back().push("CLIENT", "SETNAME", *cfg_.client_name);
        resps.emplace_back();
        ops.emplace_back("CLIENT SETNAME");
      }

      // Start all handshake commands. We explicitly create awaitables in order so requests are enqueued FIFO.
      std::vector<io::awaitable<void>> tasks;
      tasks.reserve(reqs.size());
      for (std::size_t i = 0; i < reqs.size(); ++i) {
        tasks.emplace_back(execute(reqs[i], resps[i]));
      }
      if (!tasks.empty()) {
        (void)co_await io::when_all(std::move(tasks));
      }

      // Validate responses (any server error aborts the connection).
      for (std::size_t i = 0; i < resps.size(); ++i) {
        if (!resps[i].has_value()) {
          throw std::system_error(make_error_code(xz::redis::error::resp3_simple_error),
                                  ops[i] + ": " + resps[i].error().msg);
        }
      }
    }
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
    pipeline_->stop(ec);
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
    pipeline_->stop();
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

}  // namespace xz::redis
