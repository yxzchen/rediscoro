#include <xz/io/error.hpp>
#include <xz/redis/detail/connection.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/resp3/node.hpp>

#include <netdb.h>
#include <cstring>

namespace xz::redis::detail {

connection::connection(io::io_context& ctx, config cfg) : ctx_{ctx}, cfg_{std::move(cfg)}, socket_{ctx_}, fsm_{cfg_} {
  gen = parser_.parse();
}

connection::~connection() { close(); }

auto connection::connect() -> io::task<void> {
  if (connected_) {
    co_return;
  }

  fsm_.reset();
  auto endpoint = io::ip::tcp_endpoint{io::ip::address_v4::from_string(cfg_.host), cfg_.port};
  co_await socket_.async_connect(endpoint, cfg_.connect_timeout);

  auto fsm_output = fsm_.on_connected();
  for (auto& action : fsm_output.actions) {
    if (std::holds_alternative<fsm_action::send_data>(action)) {
      auto& send = std::get<fsm_action::send_data>(action);
      co_await write_data(send.data);
    }
  }

  while (fsm_.current_state() != connection_state::ready) {
    auto span = parser_.prepare(1024);
    auto n = co_await socket_.async_read_some(std::span<char>{span.data(), span.size()}, cfg_.connect_timeout);
    parser_.commit(n);

    while (gen->next()) {
      auto msg_opt = gen->value();
      if (!msg_opt || msg_opt->empty()) {
        break;
      }

      auto fsm_output = fsm_.on_data_received(*msg_opt);

      for (auto& action : fsm_output.actions) {
        if (std::holds_alternative<fsm_action::send_data>(action)) {
          auto& send = std::get<fsm_action::send_data>(action);
          co_await write_data(send.data);
        } else if (std::holds_alternative<fsm_action::connection_failed>(action)) {
          auto& failed = std::get<fsm_action::connection_failed>(action);
          throw std::system_error(failed.ec, failed.reason);
        } else if (std::holds_alternative<fsm_action::connection_ready>(action)) {
          connected_ = true;
          co_return;
        }
      }
    }
  }
}

auto connection::write_data(std::string_view data) -> io::task<void> {
  co_await io::async_write(socket_, std::span<char const>{data.data(), data.size()}, cfg_.request_timeout);
}

auto connection::execute(request const& req) -> io::task<generic_response> {
  if (!connected_) {
    throw std::system_error(make_error_code(error::not_connected));
  }

  if (req.expected_responses() == 0) {
    co_return generic_response{std::vector<resp3::node>{}};
  }

  auto expected = req.expected_responses();
  co_await write_data(req.payload());
  pipeline_.push(expected, cfg_.request_timeout);

  pending_operation op;
  op.expected_count = expected;

  struct awaitable {
    pending_operation* op_;
    std::deque<pending_operation>* pending_ops_;

    auto await_ready() const noexcept -> bool { return false; }

    void await_suspend(std::coroutine_handle<> h) {
      op_->awaiter = h;
      pending_ops_->push_back(std::move(*op_));
    }

    auto await_resume() -> generic_response {
      auto& completed = pending_ops_->front();
      if (completed.error) {
        auto ec = completed.error;
        auto msg = ec.message();
        pending_ops_->pop_front();
        throw std::system_error(ec, msg);
      }
      auto responses = std::move(completed.responses);
      pending_ops_->pop_front();
      return generic_response{std::move(responses)};
    }
  };

  if (!read_loop_running_) {
    read_loop_running_ = true;
    ctx_.post([this]() { read_loop(); });
  }

  co_return co_await awaitable{&op, &pending_ops_};
}

auto connection::read_loop() -> io::task<void> {
  try {
    while (connected_) {
      auto span = parser_.prepare(4096);
      auto n = co_await socket_.async_read_some(std::span<char>{span.data(), span.size()}, {});

      if (n == 0) {
        complete_pending(io::error::eof);
        connected_ = false;
        break;
      }

      parser_.commit(n);

      while (gen->next()) {
        auto msg_opt = gen->value();
        if (!msg_opt || msg_opt->empty()) {
          break;
        }

        if (pending_ops_.empty() || pipeline_.empty()) {
          continue;
        }

        auto& pending = pending_ops_.front();
        for (auto const& node : *msg_opt) {
          pending.responses.push_back(resp3::to_owning_node(node));
        }

        if (pending.responses.size() >= pending.expected_count) {
          pipeline_.pop();
          if (pending.awaiter) {
            auto h = std::exchange(pending.awaiter, {});
            h.resume();
          }
        }
      }

      check_timeouts();
    }
  } catch (std::system_error const& e) {
    complete_pending(e.code());
    connected_ = false;
  } catch (...) {
    complete_pending(make_error_code(error::resp3_protocol));
    connected_ = false;
  }

  read_loop_running_ = false;
}

void connection::complete_pending(std::error_code ec) {
  for (auto& op : pending_ops_) {
    op.error = ec;
    if (op.awaiter) {
      auto h = std::exchange(op.awaiter, {});
      h.resume();
    }
  }
  pipeline_.clear();
}

void connection::check_timeouts() {
  auto now = std::chrono::steady_clock::now();
  auto timed_out = pipeline_.find_timed_out(now);
  if (timed_out.has_value()) {
    complete_pending(make_error_code(error::pong_timeout));
    connected_ = false;
    socket_.close();
  }
}

void connection::close() {
  if (connected_) {
    connected_ = false;
    complete_pending(io::error::operation_aborted);
    socket_.close();
  }
}

auto connection::is_connected() const -> bool { return connected_; }

}  // namespace xz::redis::detail
