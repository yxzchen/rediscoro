#pragma once

#include <xz/io/co_spawn.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/tcp_socket.hpp>
#include <xz/redis/adapter/any_adapter.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/resp3/parser.hpp>

#include <coroutine>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace xz::redis {

namespace detail {
class pipeline;  // internal: request scheduling / response dispatch
}

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
  enum class state {
    idle,
    connecting,
    running,
    stopped,
    failed,
  };

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
   *
   * @warning Calling run() more than once is undefined behavior.
   *          Concretely: calling run() while not in state::idle/state::stopped/state::failed is UB.
   *          (In debug builds we assert; in release builds behavior is unspecified.)
   */
  auto run() -> io::awaitable<void>;

  /// Execute a request and adapt its responses into `resp`.
  ///
  /// If `resp` is omitted, defaults to `std::ignore` (errors still propagate).
  template <class Response = ignore_t>
  auto execute(request const& req, Response& resp = std::ignore) -> io::awaitable<void> {
    co_await execute_any(req, adapter::any_adapter{resp});
  }

  void stop();
  [[nodiscard]] auto current_state() const noexcept -> state { return state_; }
  [[nodiscard]] auto is_running() const noexcept -> bool { return state_ == state::running; }
  auto error() const -> std::error_code;

  auto get_executor() noexcept -> io::io_context& { return ctx_; }

 private:
  auto ensure_pipeline() -> void;
  void close_transport() noexcept;

  auto async_write(request const& req) -> io::awaitable<void>;

  auto execute_any(request const& req, adapter::any_adapter adapter) -> io::awaitable<void>;

  auto read_loop() -> io::awaitable<void>;
  void fail(std::error_code ec);

  [[nodiscard]] auto is_inactive_state() const noexcept -> bool {
    return state_ == state::idle || state_ == state::stopped || state_ == state::failed;
  }

 private:
  state state_{state::idle};
  config cfg_;
  std::error_code error_;

  io::io_context& ctx_;
  io::tcp_socket socket_;
  resp3::parser parser_;
  std::shared_ptr<detail::pipeline> pipeline_{};
};

}  // namespace xz::redis
