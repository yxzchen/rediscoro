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
#include <coroutine>
#include <deque>
#include <optional>
#include <string>
#include <system_error>

namespace xz::redis::detail {

// FSM events
namespace fsm_event {
struct connected {};
struct disconnected {};
struct io_error {
  std::error_code ec;
};
struct msg_received {
  std::vector<resp3::node_view> msg;
};
struct timeout {};
}  // namespace fsm_event

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
  struct pending_response {
    std::coroutine_handle<> awaiter;
    adapter::any_adapter adapter;
    std::size_t expected_count;
    std::size_t received_count = 0;
    std::error_code error;
  };

  auto read_loop() -> io::task<void>;
  auto write_data(std::string_view data) -> io::task<void>;

  void start_read_loop_if_needed();
  void dispatch(fsm_event::connected event);
  void dispatch(fsm_event::io_error event);
  void dispatch(fsm_event::msg_received event);
  void dispatch(fsm_event::timeout event);
  void execute_actions(fsm_output const& out);

  auto wait_fsm_ready() -> io::task<void>;
  void setup_connect_timer();
  void cancel_connect_timer();

  io::io_context& ctx_;
  config cfg_;
  io::tcp_socket socket_;
  connection_fsm fsm_;
  resp3::parser parser_;

  bool connected_ = false;
  std::optional<io::task<void>> read_loop_task_;
  bool read_loop_running_ = false;

  // For wait_fsm_ready
  std::coroutine_handle<> connect_awaiter_;
  std::error_code connect_error_;

  // For exec
  std::deque<pending_response> pending_responses_;

  // Connect timeout timer
  io::detail::timer_handle connect_timer_;

  // Pending write tasks
  std::vector<io::task<void>> write_tasks_;
};

}  // namespace xz::redis::detail
