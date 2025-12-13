#pragma once

#include <xz/io/io_context.hpp>
#include <xz/io/ip.hpp>
#include <xz/io/task.hpp>
#include <xz/io/tcp_socket.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/detail/connection_fsm.hpp>
#include <xz/redis/adapter/any_adapter.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/response.hpp>
#include <xz/redis/resp3/parser.hpp>

#include <chrono>
#include <string>
#include <system_error>

namespace xz::redis::detail {

class connection {
 public:
  connection(io::io_context& ctx, config cfg);
  ~connection();

  connection(connection const&) = delete;
  auto operator=(connection const&) -> connection& = delete;
  connection(connection&&) = delete;
  auto operator=(connection&&) -> connection& = delete;

  auto connect() -> io::task<void>;

  template <class Response>
  auto exec(request const& req, Response& resp) -> io::task<void>;

  void close();
  auto is_connected() const -> bool;

 private:
  auto write_data(std::string_view data) -> io::task<void>;

  io::io_context& ctx_;
  config cfg_;
  io::tcp_socket socket_;
  connection_fsm fsm_;
  resp3::parser parser_;

  bool connected_ = false;
};

template <class Response>
auto connection::exec(request const& req, Response& resp) -> io::task<void> {
  if (!connected_) {
    throw std::system_error(make_error_code(error::not_connected));
  }

  if (req.expected_responses() == 0) {
    co_return;
  }

  // Write request
  co_await write_data(req.payload());

  // Read responses
  auto gen = parser_.parse();
  adapter::any_adapter adapter{resp};
  std::size_t received = 0;

  while (received < req.expected_responses()) {
    auto span = parser_.prepare(4096);
    auto n = co_await socket_.async_read_some(std::span<char>{span.data(), span.size()}, cfg_.request_timeout);

    if (n == 0) {
      throw std::system_error(io::error::eof);
    }

    parser_.commit(n);

    while (gen.next()) {
      auto msg_opt = gen.value();
      if (!msg_opt || msg_opt->empty()) {
        break;
      }

      std::error_code ec;
      adapter.on_msg(*msg_opt, ec);
      if (ec) {
        throw std::system_error(ec);
      }

      received++;
      if (received >= req.expected_responses()) {
        break;
      }
    }
  }
}

}  // namespace xz::redis::detail
