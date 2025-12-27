#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/tcp_socket.hpp>
#include <rediscoro/adapter/any_adapter.hpp>
#include <rediscoro/config.hpp>
#include <rediscoro/request.hpp>
#include <rediscoro/resp3/parser.hpp>

#include <coroutine>
#include <memory>
#include <optional>
#include <system_error>

namespace rediscoro::detail {

class pipeline;

/// Internal connection state that outlives the public `connection` handle.
///
/// - Owned by `shared_ptr`, kept alive by running coroutines
/// - Captures `shared_from_this()` in background tasks (read_loop, reconnect_loop)
/// - Destructor doesn't block, just cancels tasks
class connection_impl : public std::enable_shared_from_this<connection_impl> {
 public:
  enum class state {
    idle,
    connecting,
    running,
    stopped,
    failed,
  };

  connection_impl(xz::io::io_context& ctx, config cfg);
  ~connection_impl();

  connection_impl(connection_impl const&) = delete;
  auto operator=(connection_impl const&) -> connection_impl& = delete;
  connection_impl(connection_impl&&) = delete;
  auto operator=(connection_impl&&) -> connection_impl& = delete;

  auto run() -> xz::io::awaitable<void>;

  template <class Response>
  auto execute(request const& req, Response& resp) -> xz::io::awaitable<void> {
    co_await execute_any(req, adapter::any_adapter{resp});
  }

  auto execute_any(request const& req, adapter::any_adapter adapter) -> xz::io::awaitable<void>;

  void stop();
  auto graceful_stop() -> xz::io::awaitable<void>;

  [[nodiscard]] auto current_state() const noexcept -> state;
  [[nodiscard]] auto is_running() const noexcept -> bool;
  auto error() const -> std::error_code;
  auto get_executor() noexcept -> xz::io::io_context&;

 private:
  auto ensure_pipeline() -> void;
  auto handshake() -> xz::io::awaitable<void>;
  auto async_write(request const& req) -> xz::io::awaitable<void>;

  auto read_loop() -> xz::io::awaitable<void>;
  auto reconnect_loop() -> xz::io::awaitable<void>;

  void fail(std::error_code ec);
  void close_transport() noexcept;

  [[nodiscard]] auto is_inactive_state() const noexcept -> bool {
    return state_ == state::idle || state_ == state::stopped || state_ == state::failed;
  }

 private:
  state state_{state::idle};
  config cfg_;
  std::error_code error_;

  xz::io::io_context& ctx_;
  xz::io::tcp_socket socket_;
  resp3::parser parser_;
  std::shared_ptr<detail::pipeline> pipeline_{};

  bool reconnect_active_{false};
  std::optional<xz::io::awaitable<void>> reconnect_task_{};
  std::optional<xz::io::awaitable<void>> read_task_{};
};

}  // namespace rediscoro::detail
