#pragma once

#include <xz/io/awaitable.hpp>
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

using write_fn_t = std::function<io::awaitable<void>(request const&)>;
using error_fn_t = std::function<void(std::error_code)>;

/// Configuration for `detail::pipeline`.
struct pipeline_config {
  io::io_context& ex;
  write_fn_t write_fn;
  error_fn_t error_fn;
  std::chrono::milliseconds request_timeout{};
  std::size_t max_inflight = 0;  // 0 = unlimited
};

/// A request scheduler / pipeline with FIFO multiplexing.
///
/// Responsibilities:
/// - Write requests in-order to the socket.
/// - Maintain FIFO inflight queue (responses are FIFO in Redis/RESP).
/// - Dispatch incoming RESP messages to inflight_.front().
/// - Resume awaiting coroutines on completion.
/// - Fan out connection-level errors to all pending requests.
///
/// Non-goals (for now):
/// - Cancellation tokens
/// - Pub/Sub push handling
class pipeline : public std::enable_shared_from_this<pipeline> {
 public:
  explicit pipeline(pipeline_config const& cfg);
  ~pipeline();

  pipeline(pipeline const&) = delete;
  auto operator=(pipeline const&) -> pipeline& = delete;
  pipeline(pipeline&&) = delete;
  auto operator=(pipeline&&) -> pipeline& = delete;

  [[nodiscard]] auto stopped() const noexcept -> bool { return stopped_; }

  /// Execute a request and dispatch responses into `adapter`.
  /// This is the non-template entrypoint used by `connection::execute_any()`.
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
    io::detail::timer_handle timeout_handle{};

    std::error_code ec{};
    bool done = false;

    std::coroutine_handle<> waiter{};
    io::io_context* ex = nullptr;

    void finish(std::error_code e = {}) {
      if (done) return;
      done = true;
      if (!ec) ec = e;
      if (timeout_handle) {
        ex->cancel_timer(timeout_handle);
        timeout_handle.reset();
      }
      if (waiter) {
        auto h = waiter;
        waiter = {};
        ex->post([h]() mutable { h.resume(); });
      }
    }
  };

  struct op_awaiter {
    std::shared_ptr<op_state> op{};

    auto await_ready() const noexcept -> bool { return !op || op->done; }
    auto await_suspend(std::coroutine_handle<> h) noexcept -> bool {
      op->waiter = h;
      return true;
    }
    void await_resume() const noexcept {}
  };

  void notify_pump();
  void pump();
  void start_write_one(std::shared_ptr<op_state> const& op);
  void set_timeout(std::shared_ptr<op_state> const& op);
  void on_timeout(std::shared_ptr<op_state> const& op);

  void stop_impl(std::error_code ec, bool call_error_fn);
  void finish_all(std::error_code ec);

 private:
  io::io_context& ex_;
  write_fn_t write_fn_;
  error_fn_t error_fn_;
  std::chrono::milliseconds request_timeout_{};
  std::size_t max_inflight_ = 0;  // 0 = unlimited

  bool stopped_ = false;
  bool writing_ = false;
  bool pump_scheduled_ = false;

  std::deque<std::shared_ptr<op_state>> pending_{};
  std::deque<std::shared_ptr<op_state>> inflight_{};
};

}  // namespace xz::redis::detail
