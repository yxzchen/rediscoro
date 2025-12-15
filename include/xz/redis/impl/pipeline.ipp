#include <xz/redis/detail/pipeline.hpp>

#include <xz/io/co_spawn.hpp>
#include <xz/io/error.hpp>
#include <xz/redis/resp3/type.hpp>

namespace xz::redis::detail {

pipeline::pipeline(io::io_context& ex,
                   write_fn_t write_fn,
                   error_fn_t error_fn,
                   std::chrono::milliseconds request_timeout,
                   std::size_t max_inflight)
    : ex_{ex},
      write_fn_{std::move(write_fn)},
      error_fn_{std::move(error_fn)},
      request_timeout_{request_timeout},
      max_inflight_{max_inflight} {}

pipeline::~pipeline() {
  stop();
}

auto pipeline::execute_any(request const& req, adapter::any_adapter adapter) -> io::awaitable<void> {
  if (stopped_) {
    throw std::system_error(io::error::operation_aborted);
  }

  auto op = std::make_shared<op_state>();
  op->req = &req;
  op->adapter = std::move(adapter);
  op->remaining = req.expected_responses();
  op->timeout = request_timeout_;
  op->ex = &ex_;

  pending_.push_back(op);
  notify_pump();

  co_await op_awaiter{this, op};

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
    if (!op->failed) {
      op->adapter.on_msg(msg);
    }
  } catch (...) {
    stop_impl(io::error::operation_failed, true);
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

void pipeline::stop(std::error_code ec) {
  stop_impl(ec, false);
}

void pipeline::notify_pump() {
  if (stopped_ || pump_scheduled_) return;
  pump_scheduled_ = true;

  auto self = shared_from_this();
  ex_.post([self]() mutable {
    self->pump_scheduled_ = false;
    self->pump();
  });
}

void pipeline::pump() {
  if (stopped_) return;

  // Only one outstanding write at a time (preserve strict write ordering).
  if (writing_) return;

  // Respect max_inflight_ (0 = unlimited).
  if (pending_.empty()) return;
  if (max_inflight_ != 0 && inflight_.size() >= max_inflight_) return;

  // Move one op to inflight and start its write. This ensures responses will have a FIFO target
  // even if they arrive immediately after the coroutine yields.
  auto op = std::move(pending_.front());
  pending_.pop_front();

  inflight_.push_back(op);
  arm_timeout(op);

  writing_ = true;
  start_write_one(op);
}

void pipeline::start_write_one(std::shared_ptr<op_state> const& op) {
  auto self = shared_from_this();
  io::co_spawn(
      ex_,
      [self, op]() -> io::awaitable<void> {
        try {
          co_await self->write_fn_(*op->req);
        } catch (std::system_error const& e) {
          self->stop_impl(e.code(), true);
          co_return;
        } catch (...) {
          self->stop_impl(io::error::operation_failed, true);
          co_return;
        }

        self->writing_ = false;
        self->notify_pump();
      },
      io::use_detached);
}

void pipeline::arm_timeout(std::shared_ptr<op_state> const& op) {
  if (op->timeout.count() <= 0) return;

  auto self = shared_from_this();
  op->timeout_handle = ex_.schedule_timer(op->timeout, [self, op]() mutable { self->on_timeout(op); });
}

void pipeline::on_timeout(std::shared_ptr<op_state> const& op) {
  if (stopped_ || !op || op->done) return;

  // Timeout while inflight is response-pollution risk => treat as connection error.
  stop_impl(io::error::timeout, true);
}

void pipeline::stop_impl(std::error_code ec, bool call_error_fn) {
  if (stopped_) return;
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

}  // namespace xz::redis::detail
