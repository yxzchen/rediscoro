#include <xz/io/error.hpp>
#include <xz/redis/detail/connection.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/resp3/node.hpp>

namespace xz::redis::detail {

connection::connection(io::io_context& ctx, config cfg)
    : ctx_{ctx},
      cfg_{std::move(cfg)},
      socket_{ctx_},
      fsm_{handshake_plan{
          .needs_hello = true,  // RESP3 by default
          .needs_auth = cfg_.password.has_value(),
          .needs_select_db = cfg_.database != 0,
          .needs_set_clientname = cfg_.client_name.has_value(),
      }},
      parser_{} {}

connection::~connection() {
  close();
}

auto connection::connect() -> io::task<void> {
  // Check if already connected by checking FSM state
  if (fsm_.current_state() == connection_state::ready) {
    co_return;
  }

  fsm_.reset();
  parser_.reset();

  // 1. TCP connect
  auto endpoint = io::ip::tcp_endpoint{io::ip::address_v4::from_string(cfg_.host), cfg_.port};
  co_await socket_.async_connect(endpoint, cfg_.connect_timeout);

  // 2. Start read loop
  start_read_loop_if_needed();

  // 3. Execute handshake
  auto actions = fsm_.on_connected();
  execute_actions(actions);

  // 4. Setup timeout
  setup_connect_timer();

  // 5. Wait for ready or error
  co_await wait_fsm_ready();
}

void connection::start_read_loop_if_needed() {
  if (!read_loop_running_) {
    read_loop_running_ = true;
    read_loop_task_ = read_loop();
    read_loop_task_->resume();
  }
}

auto connection::read_loop() -> io::task<void> {
  auto gen = parser_.parse();

  for (;;) {
    auto span = parser_.prepare(4096);
    auto n = co_await socket_.async_read_some(std::span<char>{span.data(), span.size()}, {});

    if (n == 0) {
      // EOF
      auto actions = fsm_.on_io_error(io::error::eof);
      execute_actions(actions);
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
      if (!msg_opt->empty()) {
        interpret_response(*msg_opt);
      }
    }

    // Check for parser error
    if (auto ec = parser_.error()) {
      auto actions = fsm_.on_io_error(ec);
      execute_actions(actions);
      co_return;
    }
  }
}

void connection::interpret_response(resp3::msg_view const& msg) {
  // Determine which FSM event to trigger based on current FSM state
  auto current_state = fsm_.current_state();

  // Helper to check if response indicates success
  // HELLO returns a map, AUTH/SELECT/CLIENTNAME return simple_string "OK"
  auto is_success = [&msg]() -> bool {
    auto t = msg[0].data_type;
    return t == resp3::type3::simple_string || t == resp3::type3::map;
  };

  auto is_error = [&msg]() -> bool {
    auto t = msg[0].data_type;
    return t == resp3::type3::simple_error || t == resp3::type3::blob_error;
  };

  // Only interpret responses during handshake
  // Once FSM reaches 'ready', responses go to pipeline/scheduler
  if (current_state == connection_state::ready || current_state == connection_state::disconnected ||
      current_state == connection_state::failed) {
    return;  // Not in handshake, ignore
  }

  fsm_output actions;

  // Map FSM state to the appropriate command response
  switch (current_state) {
    case connection_state::handshaking: {
      // HELLO response: map with server info, or error
      if (is_success()) {
        actions = fsm_.on_hello_ok();
      } else if (is_error()) {
        actions = fsm_.on_hello_error(make_error_code(error::resp3_hello));
      }
      break;
    }

    case connection_state::authenticating: {
      // AUTH response: simple-string "OK" or error
      if (is_success()) {
        actions = fsm_.on_auth_ok();
      } else if (is_error()) {
        actions = fsm_.on_auth_error(make_error_code(error::auth_failed));
      }
      break;
    }

    case connection_state::selecting_db: {
      // SELECT response: simple-string "OK" or error
      if (is_success()) {
        actions = fsm_.on_select_ok();
      } else if (is_error()) {
        actions = fsm_.on_select_error(make_error_code(error::select_db_failed));
      }
      break;
    }

    case connection_state::setting_clientname: {
      // CLIENT SETNAME response: simple-string "OK" or error
      if (is_success()) {
        actions = fsm_.on_clientname_ok();
      } else if (is_error()) {
        actions = fsm_.on_clientname_error(make_error_code(error::client_setname_failed));
      }
      break;
    }

    case connection_state::ready:
    case connection_state::disconnected:
    case connection_state::failed:
      // Already handled above - should not reach here
      return;
  }

  execute_actions(actions);
}

void connection::execute_actions(fsm_output const& actions) {
  for (auto const& action : actions) {
    std::visit(
        [this](auto const& a) {
          using T = std::decay_t<decltype(a)>;

          if constexpr (std::is_same_v<T, fsm_action::state_change>) {
            // State changed - FSM owns this, connection just observes
            // No action needed here - we could add logging if desired
          } else if constexpr (std::is_same_v<T, fsm_action::send_hello>) {
            // Proper fire-and-forget with lifetime management and error handling
            auto req = build_hello_request();
            auto data = std::string{req.payload()};
            ctx_.post([this, data = std::move(data)]() mutable {
              try {
                auto task = write_data(std::move(data));
                pending_writes_.push_back(std::move(task));
                pending_writes_.back().resume();
              } catch (std::system_error const& e) {
                // Write failed, report to FSM
                auto actions = fsm_.on_io_error(e.code());
                execute_actions(actions);
              }
            });

          } else if constexpr (std::is_same_v<T, fsm_action::send_auth>) {
            auto req = build_auth_request();
            auto data = std::string{req.payload()};
            ctx_.post([this, data = std::move(data)]() mutable {
              try {
                auto task = write_data(std::move(data));
                pending_writes_.push_back(std::move(task));
                pending_writes_.back().resume();
              } catch (std::system_error const& e) {
                auto actions = fsm_.on_io_error(e.code());
                execute_actions(actions);
              }
            });

          } else if constexpr (std::is_same_v<T, fsm_action::send_select>) {
            auto req = build_select_request();
            auto data = std::string{req.payload()};
            ctx_.post([this, data = std::move(data)]() mutable {
              try {
                auto task = write_data(std::move(data));
                pending_writes_.push_back(std::move(task));
                pending_writes_.back().resume();
              } catch (std::system_error const& e) {
                auto actions = fsm_.on_io_error(e.code());
                execute_actions(actions);
              }
            });

          } else if constexpr (std::is_same_v<T, fsm_action::send_clientname>) {
            auto req = build_clientname_request();
            auto data = std::string{req.payload()};
            ctx_.post([this, data = std::move(data)]() mutable {
              try {
                auto task = write_data(std::move(data));
                pending_writes_.push_back(std::move(task));
                pending_writes_.back().resume();
              } catch (std::system_error const& e) {
                auto actions = fsm_.on_io_error(e.code());
                execute_actions(actions);
              }
            });

          } else if constexpr (std::is_same_v<T, fsm_action::connection_ready>) {
            // Handshake complete, clear pending writes
            pending_writes_.clear();
            cancel_connect_timer();
            if (connect_awaiter_) {
              auto h = std::exchange(connect_awaiter_, {});
              ctx_.post([h]() mutable { h.resume(); });
            }

          } else if constexpr (std::is_same_v<T, fsm_action::connection_failed>) {
            fail_connection(a.ec);
          }
        },
        action);
  }
}

auto connection::build_hello_request() -> request {
  // HELLO 3
  request req;
  req.push("HELLO", 3);
  return req;
}

auto connection::build_auth_request() -> request {
  request req;
  if (cfg_.username.has_value()) {
    // AUTH username password
    req.push("AUTH", *cfg_.username, *cfg_.password);
  } else {
    // AUTH password
    req.push("AUTH", *cfg_.password);
  }
  return req;
}

auto connection::build_select_request() -> request {
  request req;
  req.push("SELECT", cfg_.database);
  return req;
}

auto connection::build_clientname_request() -> request {
  request req;
  req.push("CLIENT", "SETNAME", *cfg_.client_name);
  return req;
}

auto connection::write_data(std::string data) -> io::task<void> {
  co_await io::async_write(socket_, std::span<char const>{data.data(), data.size()}, cfg_.request_timeout);
}

auto connection::wait_fsm_ready() -> io::task<void> {
  struct awaitable {
    connection* conn;

    auto await_ready() const noexcept -> bool {
      return conn->fsm_.current_state() == connection_state::ready || conn->connect_error_;
    }

    void await_suspend(std::coroutine_handle<> h) {
      // Check again to avoid race condition
      if (conn->fsm_.current_state() == connection_state::ready || conn->connect_error_) {
        // Already ready, resume immediately
        h.resume();
      } else {
        // Not ready yet, store awaiter
        conn->connect_awaiter_ = h;
      }
    }

    auto await_resume() -> void {
      if (conn->connect_error_) {
        auto ec = std::exchange(conn->connect_error_, {});
        throw std::system_error(ec);
      }
    }
  };

  co_await awaitable{this};
}

void connection::setup_connect_timer() {
  if (cfg_.connect_timeout.count() > 0) {
    connect_timer_ = ctx_.schedule_timer(cfg_.connect_timeout, [this]() {
      auto actions = fsm_.on_io_error(make_error_code(error::connect_timeout));
      execute_actions(actions);
    });
  }
}

void connection::cancel_connect_timer() {
  if (connect_timer_) {
    ctx_.cancel_timer(connect_timer_);
    connect_timer_.reset();
  }
}

void connection::fail_connection(std::error_code ec) {
  cancel_connect_timer();
  if (connect_awaiter_) {
    connect_error_ = ec;
    auto h = std::exchange(connect_awaiter_, {});
    ctx_.post([h]() mutable { h.resume(); });
  }
}

void connection::close() {
  if (read_loop_running_) {
    read_loop_running_ = false;
    socket_.close();
    fsm_.reset();
    parser_.reset();
    read_loop_task_.reset();
    cancel_connect_timer();
    pending_writes_.clear();  // Cancel any pending writes

    // Clear connect awaiter if still waiting
    if (connect_awaiter_) {
      connect_error_ = io::error::operation_aborted;
      auto h = std::exchange(connect_awaiter_, {});
      h.resume();
    }
  }
}

auto connection::is_connected() const -> bool {
  return fsm_.current_state() == connection_state::ready;
}

}  // namespace xz::redis::detail
