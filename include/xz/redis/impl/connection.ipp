#include <xz/io/error.hpp>
#include <xz/redis/detail/connection.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/resp3/node.hpp>

#include <iostream>

namespace xz::redis::detail {

connection::connection(io::io_context& ctx, config cfg)
    : ctx_{ctx}, cfg_{std::move(cfg)}, socket_{ctx_}, fsm_{cfg_}, parser_{} {}

connection::~connection() {
  close();
}

auto connection::connect() -> io::task<void> {
  if (connected_) {
    co_return;
  }

  std::cerr << "connect: starting\n";
  fsm_.reset();
  parser_.reset();

  auto endpoint = io::ip::tcp_endpoint{io::ip::address_v4::from_string(cfg_.host), cfg_.port};
  co_await socket_.async_connect(endpoint, cfg_.connect_timeout);
  std::cerr << "connect: TCP connected\n";

  start_read_loop_if_needed();
  std::cerr << "connect: read_loop started\n";

  dispatch(fsm_event::connected{});
  std::cerr << "connect: dispatched connected event\n";

  setup_connect_timer();
  std::cerr << "connect: timer set up\n";

  co_await wait_fsm_ready();
  std::cerr << "connect: FSM ready\n";

  connected_ = true;
}

void connection::start_read_loop_if_needed() {
  if (!read_loop_running_) {
    read_loop_running_ = true;
    read_loop_task_ = read_loop();
    read_loop_task_->resume();
  }
}

auto connection::read_loop() -> io::task<void> {
  std::cerr << "read_loop: starting\n";
  auto gen = parser_.parse();

  for (;;) {
    std::cerr << "read_loop: preparing to read\n";
    auto span = parser_.prepare(4096);
    auto n = co_await socket_.async_read_some(std::span<char>{span.data(), span.size()}, {});
    std::cerr << "read_loop: read " << n << " bytes\n";

    if (n == 0) {
      std::cerr << "read_loop: EOF\n";
      dispatch(fsm_event::io_error{io::error::eof});
      co_return;
    }

    parser_.commit(n);

    while (gen.next()) {
      if (auto msg_opt = gen.value()) {
        if (!msg_opt->empty()) {
          std::cerr << "read_loop: dispatching message with " << msg_opt->size() << " nodes\n";
          dispatch(fsm_event::msg_received{*msg_opt});
          std::cerr << "read_loop: dispatch returned\n";
        }
      }
    }
    std::cerr << "read_loop: done processing messages\n";

    if (auto ec = parser_.error()) {
      std::cerr << "read_loop: parser error\n";
      dispatch(fsm_event::io_error{ec});
      co_return;
    }
    std::cerr << "read_loop: looping back\n";
  }
  std::cerr << "read_loop: exiting\n";
}

void connection::dispatch(fsm_event::connected event) {
  std::cerr << "dispatch: connected\n";
  auto out = fsm_.on_connected();
  execute_actions(out);
}

void connection::dispatch(fsm_event::io_error event) {
  std::cerr << "dispatch: io_error\n";
  auto out = fsm_.on_connection_failed(event.ec);
  execute_actions(out);

  // Also fail any pending responses
  for (auto& pending : pending_responses_) {
    pending.error = event.ec;
    if (pending.awaiter) {
      auto h = std::exchange(pending.awaiter, {});
      ctx_.post([h]() mutable { h.resume(); });
    }
  }

  // Fail connection awaiter if still waiting
  if (connect_awaiter_) {
    connect_error_ = event.ec;
    auto h = std::exchange(connect_awaiter_, {});
    ctx_.post([h]() mutable { h.resume(); });
  }
}

void connection::dispatch(fsm_event::msg_received event) {
  std::cerr << "dispatch: msg_received, FSM state=" << static_cast<int>(fsm_.current_state()) << "\n";
  // During connection handshake
  if (fsm_.current_state() != connection_state::ready) {
    auto out = fsm_.on_data_received(event.msg);
    std::cerr << "dispatch: executing " << out.actions.size() << " actions\n";
    execute_actions(out);
    std::cerr << "dispatch: done executing actions\n";
    return;
  }

  // During normal operation - dispatch to pending responses
  if (pending_responses_.empty()) {
    return;
  }

  auto& pending = pending_responses_.front();
  std::error_code ec;
  pending.adapter.on_msg(event.msg, ec);

  if (ec) {
    pending.error = ec;
    if (pending.awaiter) {
      auto h = std::exchange(pending.awaiter, {});
      ctx_.post([h]() mutable { h.resume(); });
    }
    return;
  }

  pending.received_count++;
  if (pending.received_count >= pending.expected_count) {
    if (pending.awaiter) {
      auto h = std::exchange(pending.awaiter, {});
      ctx_.post([h]() mutable { h.resume(); });
    }
  }
}

void connection::dispatch(fsm_event::timeout event) {
  auto out = fsm_.on_connection_failed(make_error_code(error::pong_timeout));
  execute_actions(out);

  if (connect_awaiter_) {
    connect_error_ = make_error_code(error::pong_timeout);
    auto h = std::exchange(connect_awaiter_, {});
    ctx_.post([h]() mutable { h.resume(); });
  }
}

void connection::execute_actions(fsm_output const& out) {
  for (auto const& action : out.actions) {
    if (std::holds_alternative<fsm_action::send_data>(action)) {
      std::cerr << "execute_actions: send_data\n";
      auto const& send = std::get<fsm_action::send_data>(action);
      // Store task and start it
      write_tasks_.push_back(write_data(send.data));
      write_tasks_.back().resume();
      std::cerr << "execute_actions: write task started\n";
    } else if (std::holds_alternative<fsm_action::connection_ready>(action)) {
      std::cerr << "execute_actions: connection_ready\n";
      cancel_connect_timer();
      if (connect_awaiter_) {
        auto h = std::exchange(connect_awaiter_, {});
        ctx_.post([h]() mutable { h.resume(); });
      }
    } else if (std::holds_alternative<fsm_action::connection_failed>(action)) {
      std::cerr << "execute_actions: connection_failed\n";
      auto const& failed = std::get<fsm_action::connection_failed>(action);
      cancel_connect_timer();
      if (connect_awaiter_) {
        connect_error_ = failed.ec;
        auto h = std::exchange(connect_awaiter_, {});
        ctx_.post([h]() mutable { h.resume(); });
      }
    }
  }
}

auto connection::wait_fsm_ready() -> io::task<void> {
  struct awaitable {
    connection* conn;

    auto await_ready() const noexcept -> bool {
      return conn->fsm_.current_state() == connection_state::ready || conn->connect_error_;
    }

    void await_suspend(std::coroutine_handle<> h) {
      conn->connect_awaiter_ = h;
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
      dispatch(fsm_event::timeout{});
    });
  }
}

void connection::cancel_connect_timer() {
  if (connect_timer_) {
    ctx_.cancel_timer(connect_timer_);
    connect_timer_.reset();
  }
}

auto connection::write_data(std::string_view data) -> io::task<void> {
  co_await io::async_write(socket_, std::span<char const>{data.data(), data.size()}, cfg_.request_timeout);
}

auto connection::exec(request const& req, adapter::any_adapter adapter) -> io::task<void> {
  if (!connected_) {
    throw std::system_error(make_error_code(error::not_connected));
  }

  if (req.expected_responses() == 0) {
    co_return;
  }

  // Write request
  co_await write_data(req.payload());

  // Create pending response
  pending_response pending;
  pending.adapter = std::move(adapter);
  pending.expected_count = req.expected_responses();

  struct awaitable {
    pending_response* pending_;
    std::deque<pending_response>* pending_responses_;

    auto await_ready() const noexcept -> bool { return false; }

    void await_suspend(std::coroutine_handle<> h) {
      pending_->awaiter = h;
      pending_responses_->push_back(std::move(*pending_));
    }

    auto await_resume() -> void {
      auto& completed = pending_responses_->front();
      if (completed.error) {
        auto ec = completed.error;
        pending_responses_->pop_front();
        throw std::system_error(ec);
      }
      pending_responses_->pop_front();
    }
  };

  co_await awaitable{&pending, &pending_responses_};
}

void connection::close() {
  if (connected_ || read_loop_running_) {
    connected_ = false;
    read_loop_running_ = false;
    socket_.close();
    fsm_.reset();
    parser_.reset();
    read_loop_task_.reset();
    cancel_connect_timer();
    write_tasks_.clear();

    // Clear any pending awaiters
    if (connect_awaiter_) {
      connect_error_ = io::error::operation_aborted;
      auto h = std::exchange(connect_awaiter_, {});
      h.resume();
    }

    for (auto& pending : pending_responses_) {
      if (pending.awaiter) {
        pending.error = io::error::operation_aborted;
        auto h = std::exchange(pending.awaiter, {});
        h.resume();
      }
    }
    pending_responses_.clear();
  }
}

auto connection::is_connected() const -> bool {
  return connected_;
}

}  // namespace xz::redis::detail
