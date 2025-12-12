#include <xz/io/error.hpp>
#include <xz/redis/detail/connection.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/resp3/node.hpp>

#include <netdb.h>
#include <cstring>

namespace xz::redis::detail {

connection::connection(io::io_context& ctx, config cfg)
    : ctx_{ctx}, cfg_{std::move(cfg)}, socket_{ctx_}, fsm_{cfg_}, gen_{parser_.parse()} {}

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

    while (gen_.next()) {
      auto msg_opt = gen_.value();
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

      while (gen_.next()) {
        auto msg_opt = gen_.value();
        if (!msg_opt || msg_opt->empty()) {
          break;
        }

        if (pending_ops_.empty() || pipeline_.empty()) {
          continue;
        }

        auto& pending = pending_ops_.front();
        std::error_code ec;
        pending.adapter.on_msg(*msg_opt, ec);

        if (ec) {
          pending.error = ec;
          pipeline_.pop();
          if (pending.awaiter) {
            auto h = std::exchange(pending.awaiter, {});
            h.resume();
          }
          continue;
        }

        pending.received_count++;
        if (pending.received_count >= pending.expected_count) {
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
