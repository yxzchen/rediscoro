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

  auto exec(request const& req, adapter::any_adapter adapter) -> io::task<void>;

  template <class Response>
  auto exec(request const& req, Response& resp) -> io::task<void> {
    return exec(req, adapter::any_adapter{resp});
  }

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

}  // namespace xz::redis::detail
