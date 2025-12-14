#pragma once

#include <xz/io/io_context.hpp>
#include <xz/io/ip.hpp>
#include <xz/io/tcp_socket.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/resp3/parser.hpp>

#include <coroutine>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace xz::redis::detail {

/**
 * @brief Connection handles TCP and RESP parsing
 *
 * Responsibilities:
 * - TCP connection management
 * - RESP3 parsing
 * - Read loop for incoming data
 *
 * Thread Safety:
 * - connection is NOT thread-safe
 * - All public methods must be called from the same io_context thread
 *
 * Does NOT handle:
 * - Handshake (will be implemented later with pipeline)
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
   * @brief Start the connection (TCP connect + read loop)
   *
   * Steps:
   * 1. TCP connect
   * 2. Start read loop (background)
   *
   * Post-condition:
   * - On success: TCP connection established and read loop running
   * - On failure: exception is thrown with error code
   */
  auto run() -> io::awaitable<void>;

  void stop();
  auto is_running() const -> bool;

 private:
  auto read_loop() -> io::awaitable<void>;
  void fail(std::error_code ec);

 private:
  io::io_context& ctx_;
  config cfg_;
  io::tcp_socket socket_;
  resp3::parser parser_;
  bool running_ = false;
  std::error_code error_;
};

}  // namespace xz::redis::detail
