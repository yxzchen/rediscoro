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

  template <class Response>
  auto exec(request const& req, Response& resp) -> io::task<void>;

  void close();
  auto is_connected() const -> bool;

 private:
  struct pending_operation {
    std::coroutine_handle<> awaiter;
    adapter::any_adapter adapter;
    std::size_t expected_count;
    std::size_t received_count = 0;
    std::chrono::steady_clock::time_point deadline;
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
  resp3::parser parser_;

  std::deque<pending_operation> pending_ops_;
  bool connected_ = false;
  bool read_loop_running_ = false;
  std::optional<io::task<void>> read_loop_task_;
};

template <class Response>
auto connection::exec(request const& req, Response& resp) -> io::task<void> {
  if (!connected_) {
    throw std::system_error(make_error_code(error::not_connected));
  }

  if (req.expected_responses() == 0) {
    co_return;
  }

  auto expected = req.expected_responses();
  co_await write_data(req.payload());

  pending_operation op;
  op.adapter = adapter::any_adapter{resp};
  op.expected_count = expected;
  op.deadline = std::chrono::steady_clock::now() + cfg_.request_timeout;

  struct awaitable {
    pending_operation* op_;
    std::deque<pending_operation>* pending_ops_;

    auto await_ready() const noexcept -> bool { return false; }

    void await_suspend(std::coroutine_handle<> h) {
      op_->awaiter = h;
      pending_ops_->push_back(std::move(*op_));
    }

    auto await_resume() -> void {
      auto& completed = pending_ops_->front();
      if (completed.error) {
        auto ec = completed.error;
        pending_ops_->pop_front();
        throw std::system_error(ec);
      }
      pending_ops_->pop_front();
    }
  };

  if (!read_loop_running_) {
    read_loop_running_ = true;
    read_loop_task_ = read_loop();
    read_loop_task_->resume();
  }

  co_await awaitable{&op, &pending_ops_};
}

}  // namespace xz::redis::detail
