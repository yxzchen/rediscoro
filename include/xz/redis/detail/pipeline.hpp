#pragma once

#include <xz/io/awaitable.hpp>
#include <xz/io/co_sleep.hpp>
#include <xz/io/io_context.hpp>
#include <xz/redis/adapter/any_adapter.hpp>
#include <xz/redis/detail/assert.hpp>
#include <xz/redis/ignore.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/resp3/node.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <system_error>

namespace xz::redis::detail {

/// A simple request scheduler / pipeline.
///
/// Responsibilities:
/// - Serialize concurrent execute() calls into a single in-order stream.
/// - Dispatch incoming RESP messages to the active request adapter.
/// - Resume awaiting coroutines on completion.
/// - Fan out connection-level errors to all pending requests.
///
/// Non-goals (for now):
/// - Cancellation tokens
/// - Pub/Sub push handling
/// - Pipelining multiple in-flight requests (this is strictly sequential)
class pipeline {
 public:
  using write_fn_t = std::function<io::awaitable<void>(request const&)>;
  using error_fn_t = std::function<void(std::error_code)>;

  pipeline(io::io_context& ex, write_fn_t write_fn, error_fn_t error_fn, std::chrono::milliseconds request_timeout);
  ~pipeline();

  pipeline(pipeline const&) = delete;
  auto operator=(pipeline const&) -> pipeline& = delete;
  pipeline(pipeline&&) = delete;
  auto operator=(pipeline&&) -> pipeline& = delete;

  [[nodiscard]] auto stopped() const noexcept -> bool { return stopped_; }

  /// Execute a request and adapt responses into `resp`.
  ///
  /// If `resp` is omitted, defaults to `std::ignore` (errors still propagate).
  template <class Response = ignore_t>
  auto execute(request const& req, Response& resp = std::ignore) -> io::awaitable<void> {
    co_await execute_any(req, adapter::any_adapter{resp});
  }

  /// Execute a request and dispatch responses into `adapter`.
  /// This is the non-template entrypoint used by `connection::execute()`.
  auto execute_any(request const& req, adapter::any_adapter adapter) -> io::awaitable<void>;

  /// Called by `connection` for each parsed RESP message (single-threaded: io_context thread).
  void on_msg(resp3::msg_view const& msg);

  void stop(std::error_code ec = io::error::operation_aborted);

 private:
  struct op_state {
    request const* req = nullptr;
    adapter::any_adapter adapter{};
    std::size_t remaining = 0;
    std::chrono::milliseconds timeout{};
    std::error_code ec{};
    bool done = false;

    std::coroutine_handle<> waiter_user{};
    std::coroutine_handle<> waiter_pump{};
  };

  struct queue_awaiter {
    pipeline* self = nullptr;
    auto await_ready() const noexcept -> bool { return false; }
    auto await_suspend(std::coroutine_handle<> h) noexcept -> bool {
      if (self->stopped_ || !self->pending_.empty()) {
        return false;
      }
      self->queue_waiter_ = h;
      return true;
    }
    void await_resume() const noexcept { self->queue_waiter_ = {}; }
  };

  struct op_awaiter {
    pipeline* self = nullptr;
    std::shared_ptr<op_state> op{};
    bool for_pump = false;

    auto await_ready() const noexcept -> bool { return !op || op->done; }
    auto await_suspend(std::coroutine_handle<> h) noexcept -> bool {
      if (for_pump) {
        op->waiter_pump = h;
      } else {
        op->waiter_user = h;
      }
      return true;
    }
    void await_resume() const noexcept {}
  };

  auto pump() -> io::awaitable<void>;
  auto wait_active_done(std::shared_ptr<op_state> op) -> io::awaitable<void>;

  void notify_queue();
  void complete(std::shared_ptr<op_state> const& op);
  void resume(std::coroutine_handle<> h);

  void complete_pending(std::error_code ec);

 private:
  io::io_context& ex_;
  write_fn_t write_fn_;
  error_fn_t error_fn_;
  std::chrono::milliseconds request_timeout_{};
  std::deque<std::shared_ptr<op_state>> pending_{};
  std::shared_ptr<op_state> active_{};

  std::coroutine_handle<> queue_waiter_{};
  bool stopped_ = false;
};

}  // namespace xz::redis::detail
