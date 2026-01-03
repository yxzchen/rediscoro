#include <rediscoro/detail/pipeline.hpp>

#include <iocoro/co_spawn.hpp>
#include <iocoro/error.hpp>
#include <rediscoro/resp3/type.hpp>

#include <cerrno>
#include <system_error>

namespace rediscoro::detail {

pipeline::pipeline(pipeline_config const& cfg)
    : ex_{cfg.ex},
      write_fn_{std::move(cfg.write_fn)},
      error_fn_{std::move(cfg.error_fn)},
      request_timeout_{cfg.request_timeout},
      max_inflight_{cfg.max_inflight} {}

pipeline::~pipeline() { stop_impl(iocoro::error::operation_aborted, false); }

auto pipeline::execute_any(request const& req, adapter::any_adapter adapter)
  -> iocoro::awaitable<void> {
  if (stopped_) {
    throw std::system_error(iocoro::error::operation_aborted);
  }

  auto op = std::make_shared<op_state>();
  op->req = &req;
  op->adapter = std::move(adapter);
  op->remaining = req.expected_responses();
  op->timeout = request_timeout_;
  op->ex = ex_;

  pending_.push_back(op);
  notify_pump();

  co_await op_awaiter{op};

  if (op->ec) {
    throw std::system_error(op->ec);
  }
}

void pipeline::on_msg(resp3::msg_view const& msg) {
  if (stopped_ || msg.empty()) {
    return;
  }

  // Future: push/attribute messages must not be dispatched to inflight FIFO.
  auto const t = msg.front().data_type;
  if (t == resp3::type3::push || t == resp3::type3::attribute) {
    return;
  }

  if (inflight_.empty()) {
    // Unsolicited message (unsupported for now).
    return;
  }

  auto op = inflight_.front();

  // Adapter throws => response pollution risk => treat as connection error.
  try {
    op->adapter.on_msg(msg);
  } catch (...) {
    stop_impl(std::make_error_code(std::errc::io_error), true);
    return;
  }

  if (op->remaining > 0) {
    --op->remaining;
  }

  if (op->remaining == 0) {
    inflight_.pop_front();
    op->finish();
    notify_pump();
  }
}

void pipeline::notify_pump() {
  if (stopped_ || pump_scheduled_) {
    return;
  }
  pump_scheduled_ = true;

  auto self = shared_from_this();
  ex_.post([self, ex = ex_]() mutable {
    iocoro::detail::executor_guard g{ex};
    self->pump_scheduled_ = false;
    self->pump();
  });
}

void pipeline::pump() {
  if (stopped_) {
    return;
  }

  // Only one outstanding write at a time (preserve strict write ordering).
  if (writing_) {
    return;
  }

  // Respect max_inflight_ (0 = unlimited).
  if (pending_.empty()) {
    return;
  }
  if (max_inflight_ != 0 && inflight_.size() >= max_inflight_) {
    return;
  }

  // Move one op to inflight and start its write. This ensures responses will have a FIFO target
  // even if they arrive immediately after the coroutine yields.
  auto op = std::move(pending_.front());
  pending_.pop_front();

  inflight_.push_back(op);
  set_timeout(op);

  writing_ = true;
  start_write_one(op);
}

void pipeline::start_write_one(std::shared_ptr<op_state> const& op) {
  auto self = shared_from_this();
  iocoro::co_spawn(
    ex_,
    [self, op]() -> iocoro::awaitable<void> {
      co_await self->write_fn_(*op->req);

      self->writing_ = false;
      self->notify_pump();
    },
    [self](iocoro::expected<void, std::exception_ptr> r) {
      if (r) {
        return;
      }
      try {
        std::rethrow_exception(r.error());
      } catch (std::system_error const& e) {
        self->stop_impl(e.code(), true);
      } catch (...) {
        self->stop_impl(std::make_error_code(std::errc::io_error), true);
      }
    });
}

void pipeline::set_timeout(std::shared_ptr<op_state> const& op) {
  if (op->timeout.count() <= 0) {
    return;
  }

  // Create timer with shared ownership for lifetime management
  op->timer = std::make_shared<iocoro::steady_timer>(ex_);
  op->timer->expires_after(op->timeout);

  // Spawn a detached coroutine to wait for the timer
  auto self = shared_from_this();
  auto timer = op->timer;  // Capture shared_ptr by value

  iocoro::co_spawn(
    ex_,
    [timer, self, op]() -> iocoro::awaitable<void> {
      auto ec = co_await timer->async_wait(iocoro::use_awaitable);
      if (!ec) {
        self->on_timeout(op);
      }
      // Timer is automatically destroyed after callback completes
    },
    [](iocoro::expected<void, std::exception_ptr>) {
      // Completion handler - we don't care about the result
    }
  );
}

void pipeline::on_timeout(std::shared_ptr<op_state> const& op) {
  if (stopped_ || !op || op->done) {
    return;
  }

  // Timeout while inflight is response-pollution risk => treat as connection error.
  stop_impl(iocoro::error::timed_out, true);
}

void pipeline::stop(std::error_code ec) { stop_impl(ec, false); }

void pipeline::stop_impl(std::error_code ec, bool call_error_fn) {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  writing_ = false;

  finish_all(ec);

  if (call_error_fn && error_fn_) {
    try {
      error_fn_(ec);
    } catch (...) {
      // best-effort
    }
  }
}

void pipeline::finish_all(std::error_code ec) {
  // Complete inflight first.
  while (!inflight_.empty()) {
    auto op = std::move(inflight_.front());
    inflight_.pop_front();
    op->finish(ec);
  }
  while (!pending_.empty()) {
    auto op = std::move(pending_.front());
    pending_.pop_front();
    op->finish(ec);
  }
}

}  // namespace rediscoro::detail
