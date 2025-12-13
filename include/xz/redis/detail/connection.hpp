#pragma once

#include <xz/io/io_context.hpp>
#include <xz/io/ip.hpp>
#include <xz/io/task.hpp>
#include <xz/io/tcp_socket.hpp>
#include <xz/io/co_spawn.hpp>
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
 * Design principles:
 * - FSM (connection_fsm) is the ONLY source of truth for handshake state
 * - connection is responsible for execution (I/O, parsing, command serialization)
 * - connection does NOT track handshake state - FSM owns that
 *
 * Responsibilities:
 * - TCP connection management
 * - RESP3 parsing
 * - FSM action execution (command serialization via request class)
 * - Semantic event interpretation (RESP → FSM events)
 * - Timeout handling
 *
 * Post-condition of connect():
 * When connect() coroutine returns successfully:
 * - socket is connected and active
 * - handshake FSM has reached 'ready' state
 * - read_loop is running in background
 * - All errors are propagated as exceptions
 *
 * Thread Safety:
 * - connection is NOT thread-safe
 * - All public methods must be called from the same io_context thread
 * - This ensures thread-safe access to write_queue_ and FSM state
 *
 * FSM Contract for Handshake:
 * - Responses are assumed to arrive in request order
 * - No interleaved push messages or ACL warnings are expected during handshake
 * - Once FSM enters 'failed' state, no more writes or handshake actions are allowed
 *
 * Does NOT handle:
 * - User request queueing (handled by pipeline/scheduler)
 * - Response dispatching (handled by pipeline/scheduler)
 * - Concurrent writes during handshake (handshake writes are sequential)
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
   * 2. Start read loop (background)
   * 3. Execute handshake sequence (HELLO/AUTH/SELECT/CLIENT SETNAME)
   * 4. Wait for FSM to reach ready state
   *
   * Post-condition:
   * - On success: connection is ready to accept user requests
   * - On failure: exception is thrown with error code
   *
   * Constraints:
   * - This is a SINGLE-WAITER operation
   * - Do not call connect() concurrently on the same connection
   * - wait_fsm_ready() supports only one awaiting coroutine
   */
  auto connect() -> io::task<void>;

  void close();
  auto is_connected() const -> bool;

 private:
  // === Core coroutines ===
  auto read_loop() -> io::task<void>;
  auto write_data(std::string data) -> io::task<void>;

  // === FSM integration ===
  /**
   * @brief Execute FSM actions
   *
   * FSM actions are authoritative and describe WHAT to do.
   * This function executes them (HOW to do it).
   *
   * This does NOT track handshake state - FSM owns that.
   * This only performs I/O operations and invokes callbacks.
   *
   * FSM may output multiple send_* actions in sequence.
   * These are queued for sequential execution (pipelining is safe for handshake).
   * Responses are assumed to arrive in the same order as requests.
   */
  void execute_actions(fsm_output const& actions);

  /**
   * @brief Interpret RESP3 response and convert to FSM events
   *
   * This function:
   * - Does NOT do RESP parsing (already done by parser)
   * - Does NOT do I/O
   * - Does interpret the semantic meaning of responses
   * - Converts success/error to appropriate FSM callbacks
   *
   * Responsibility: Map RESP3 message → FSM event
   */
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

 private:
  // Awaitable for wait_fsm_ready
  // Defined as a nested class to avoid linkage issues with local structs
  struct wait_fsm_ready_awaitable {
    connection* conn;

    auto await_ready() const noexcept -> bool;
    void await_suspend(std::coroutine_handle<> h);
    auto await_resume() -> void;
  };

  // === Members ===
  io::io_context& ctx_;
  config cfg_;
  io::tcp_socket socket_;
  connection_fsm fsm_;
  resp3::parser parser_;

  std::optional<io::task<void>> read_loop_task_;
  bool read_loop_running_ = false;

  // For wait_fsm_ready
  std::coroutine_handle<> connect_awaiter_;
  std::error_code connect_error_;

  // Connect timeout timer
  io::detail::timer_handle connect_timer_;
};

}  // namespace xz::redis::detail
