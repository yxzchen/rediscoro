#pragma once

#include <xz/io/io_context.hpp>
#include <xz/io/ip.hpp>
#include <xz/io/task.hpp>
#include <xz/io/tcp_socket.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/detail/connection_fsm.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/resp3/parser.hpp>

#include <coroutine>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace xz::redis::detail {

/**
 * @brief Connection handles TCP, RESP, and handshake orchestration
 *
 * Responsibilities:
 * - TCP connection management
 * - RESP3 parsing
 * - FSM action execution (command serialization via request class)
 * - Semantic event interpretation (RESP → FSM events)
 * - Timeout handling
 *
 * Does NOT handle:
 * - User request queueing (handled by pipeline/scheduler)
 * - Response dispatching (handled by pipeline/scheduler)
 */
class connection {
 public:
  connection(io::io_context& ctx, config cfg);
  ~connection();

  connection(connection const&) = delete;
  auto operator=(connection const&) -> connection& = delete;
  connection(connection&&) = delete;
  auto operator=(connection&&) -> connection& = delete;

  /**
   * @brief Connect and complete handshake
   *
   * Steps:
   * 1. TCP connect
   * 2. Start read loop
   * 3. Execute handshake sequence (HELLO/AUTH/SELECT/CLIENT SETNAME)
   * 4. Wait for FSM to reach ready state
   */
  auto connect() -> io::task<void>;

  void close();
  auto is_connected() const -> bool;

 private:
  // === Handshake state tracking ===
  enum class handshake_step {
    none,
    hello,
    auth,
    select_db,
    clientname,
  };

  // === Core coroutines ===
  auto read_loop() -> io::task<void>;
  auto write_data(std::string data) -> io::task<void>;

  // === FSM integration ===
  void execute_actions(fsm_output const& actions);
  void interpret_response(resp3::msg_view const& msg);

  // === Command building (using request class) ===
  auto build_hello_request() -> request;
  auto build_auth_request() -> request;
  auto build_select_request() -> request;
  auto build_clientname_request() -> request;

  // === Helpers ===
  void start_read_loop_if_needed();
  auto wait_fsm_ready() -> io::task<void>;
  void setup_connect_timer();
  void cancel_connect_timer();
  void fail_connection(std::error_code ec);

  // === Members ===
  io::io_context& ctx_;
  config cfg_;
  io::tcp_socket socket_;
  connection_fsm fsm_;
  resp3::parser parser_;

  bool connected_ = false;
  std::optional<io::task<void>> read_loop_task_;
  bool read_loop_running_ = false;

  // Handshake tracking
  handshake_step current_step_ = handshake_step::none;

  // For wait_fsm_ready
  std::coroutine_handle<> connect_awaiter_;
  std::error_code connect_error_;

  // Connect timeout timer
  io::detail::timer_handle connect_timer_;

  // Pending write tasks
  std::vector<io::task<void>> write_tasks_;
};

}  // namespace xz::redis::detail
