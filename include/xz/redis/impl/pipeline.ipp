#include <xz/redis/detail/pipeline.hpp>

#include <xz/io/co_spawn.hpp>
#include <xz/io/error.hpp>
#include <xz/io/io_context.hpp>
#include <xz/io/when_any.hpp>

namespace xz::redis::detail {

pipeline::pipeline(io::io_context& ex, write_fn_t write_fn, error_fn_t error_fn,
                   std::chrono::milliseconds request_timeout)
    : ex_{ex}, write_fn_{std::move(write_fn)}, error_fn_{std::move(error_fn)}, request_timeout_{request_timeout} {
  io::co_spawn(ex_, pump(), io::use_detached);
}

pipeline::~pipeline() { stop(); }

auto pipeline::execute_any(request const& req, adapter::any_adapter adapter) -> io::awaitable<void> {
  if (stopped_) {
    throw std::system_error(io::error::operation_aborted);
  }

  auto op = std::make_shared<op_state>();
  op->req = &req;
  op->adapter = std::move(adapter);
  op->remaining = req.expected_responses();
  op->timeout = request_timeout_;

  pending_.push_back(op);
  notify_queue();

  co_await op_awaiter{this, op, false};

  if (op->ec) {
    throw std::system_error(op->ec);
  }
}

auto pipeline::pump() -> io::awaitable<void> {
  for (;;) {
    co_await queue_awaiter{this};
    if (stopped_) {
      co_return;
    }

    // Take next op.
    auto op = std::move(pending_.front());
    pending_.pop_front();

    active_ = op;

    try {
      // Write request payload (timeout handled by connection).
      co_await write_fn_(*op->req);
    } catch (std::system_error const& e) {
      stop(e.code());
      error_fn_(e.code());
      co_return;
    }

    // Requests with 0 expected responses (e.g. SUBSCRIBE) are not supported yet.
    if (op->remaining == 0) {
      complete(op);
      active_.reset();
      continue;
    }

    // Wait until on_msg() consumes all expected responses, or request timeout fires.
    if (op->timeout.count() == 0) {
      co_await op_awaiter{this, op, true};
    } else {
      io::sleep_operation sleep{ex_, op->timeout};
      auto [idx, _] = co_await io::when_any(wait_active_done(op), sleep.wait());

      if (idx == 1) {
        stop(io::error::timeout);
        error_fn_(io::error::timeout);
        co_return;
      }

      // Cancel losing timer so it doesn't keep the io_context alive until expiry.
      sleep.cancel();
    }
    active_.reset();
  }
}

auto pipeline::wait_active_done(std::shared_ptr<op_state> op) -> io::awaitable<void> {
  co_await op_awaiter{this, std::move(op), true};
}

void pipeline::on_msg(resp3::msg_view const& msg) {
  if (stopped_) {
    return;
  }

  auto op = active_;
  if (!op) {
    // No active request: treat as server push / unsolicited message (unsupported for now).
    return;
  }

  std::error_code ec{};
  if (!op->ec) {
    op->adapter.on_msg(msg, ec);
    if (ec) {
      stop(ec);
      error_fn_(ec);
      return;
    }
  }

  if (op->remaining > 0) {
    --op->remaining;
  }

  if (op->remaining == 0) {
    complete(op);
  }
}

void pipeline::stop(std::error_code ec) {
  if (stopped_) {
    return;
  }

  stopped_ = true;
  notify_queue();
  complete_pending(ec);
}

void pipeline::complete_pending(std::error_code ec) {
  if (active_ && !active_->done) {
    active_->ec = ec;
    complete(active_);
  }
  while (!pending_.empty()) {
    auto op = std::move(pending_.front());
    pending_.pop_front();
    op->ec = ec;
    complete(op);
  }
}

void pipeline::notify_queue() {
  if (queue_waiter_) {
    auto h = queue_waiter_;
    queue_waiter_ = {};
    resume(h);
  }
}

void pipeline::complete(std::shared_ptr<op_state> const& op) {
  if (!op || op->done) {
    return;
  }
  op->done = true;

  if (op->waiter_user) {
    auto h = op->waiter_user;
    op->waiter_user = {};
    resume(h);
  }
  if (op->waiter_pump) {
    auto h = op->waiter_pump;
    op->waiter_pump = {};
    resume(h);
  }
}

void pipeline::resume(std::coroutine_handle<> h) {
  if (!h) return;
  ex_.post([h]() mutable { h.resume(); });
}

}  // namespace xz::redis::detail
