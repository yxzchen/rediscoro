#pragma once

#include <xz/io/io_context.hpp>
#include <xz/io/ip.hpp>
#include <xz/io/task.hpp>
#include <xz/io/tcp_socket.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/detail/connection_fsm.hpp>
#include <xz/redis/detail/pipeline.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/response.hpp>
#include <xz/redis/resp3/parser.hpp>

#include <chrono>
#include <coroutine>
#include <deque>
#include <memory>
#include <optional>
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
  auto execute(request const& req) -> io::task<generic_response>;
  void close();
  auto is_connected() const -> bool;

 private:
  struct pending_operation {
    std::coroutine_handle<> awaiter;
    std::vector<resp3::node> responses;
    std::size_t expected_count;
    std::error_code error;
  };

  auto read_loop() -> io::task<void>;
  auto write_data(std::string_view data) -> io::task<void>;
  void complete_pending(std::error_code ec);
  void check_timeouts();

  io::io_context& ctx_;
  config cfg_;
  io::tcp_socket socket_;
  connection_fsm fsm_;
  pipeline pipeline_;
  resp3::parser parser_;
  std::optional<xz::redis::resp3::generator_type> gen;

  std::deque<pending_operation> pending_ops_;
  bool connected_ = false;
  bool read_loop_running_ = false;
  std::optional<io::detail::timer_handle> timeout_timer_;
};

}  // namespace xz::redis::detail
