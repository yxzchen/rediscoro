#include <xz/io/error.hpp>
#include <xz/redis/detail/connection.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/resp3/node.hpp>

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

  fsm_.reset();
  auto endpoint = io::ip::tcp_endpoint{io::ip::address_v4::from_string(cfg_.host), cfg_.port};
  co_await socket_.async_connect(endpoint, cfg_.connect_timeout);

  // Handle connection handshake (HELLO, AUTH, etc.)
  auto fsm_output = fsm_.on_connected();
  for (auto& action : fsm_output.actions) {
    if (std::holds_alternative<fsm_action::send_data>(action)) {
      auto& send = std::get<fsm_action::send_data>(action);
      co_await write_data(send.data);
    }
  }

  // Read handshake responses
  auto gen = parser_.parse();
  while (fsm_.current_state() != connection_state::ready) {
    auto span = parser_.prepare(1024);
    auto n = co_await socket_.async_read_some(std::span<char>{span.data(), span.size()}, cfg_.connect_timeout);

    if (n == 0) {
      throw std::system_error(io::error::eof);
    }

    parser_.commit(n);

    while (gen.next()) {
      auto msg_opt = gen.value();
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

  connected_ = true;
}

auto connection::write_data(std::string_view data) -> io::task<void> {
  co_await io::async_write(socket_, std::span<char const>{data.data(), data.size()}, cfg_.request_timeout);
}

void connection::close() {
  if (connected_) {
    connected_ = false;
    socket_.close();
    fsm_.reset();
  }
}

auto connection::is_connected() const -> bool {
  return connected_;
}

}  // namespace xz::redis::detail
