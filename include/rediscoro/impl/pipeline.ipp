#pragma once

#include <rediscoro/detail/pipeline.hpp>

namespace rediscoro::detail {

inline auto pipeline::push(request req, std::shared_ptr<response_sink> sink) -> void {
  push(std::move(req), sink, time_point::max());
}

inline auto pipeline::push(request req, std::shared_ptr<response_sink> sink,
                           time_point deadline) -> void {
  REDISCORO_ASSERT(sink != nullptr);
  REDISCORO_ASSERT(req.reply_count() == sink->expected_replies());

  pending_write_.push_back(pending_item{std::move(req), std::move(sink), 0, deadline});
}

inline bool pipeline::has_pending_write() const noexcept {
  return !pending_write_.empty();
}

inline bool pipeline::has_pending_read() const noexcept {
  return !awaiting_read_.empty();
}

inline auto pipeline::next_write_buffer() -> std::string_view {
  REDISCORO_ASSERT(!pending_write_.empty());
  auto& front = pending_write_.front();
  const auto& wire = front.req.wire();
  REDISCORO_ASSERT(front.written <= wire.size());
  return std::string_view{wire}.substr(front.written);
}

inline auto pipeline::on_write_done(std::size_t n) -> void {
  REDISCORO_ASSERT(!pending_write_.empty());
  auto& front = pending_write_.front();
  const auto& wire = front.req.wire();
  REDISCORO_ASSERT(front.written <= wire.size());
  REDISCORO_ASSERT(n <= (wire.size() - front.written));

  front.written += n;

  if (front.written == wire.size()) {
    // Entire request written: move to awaiting read queue.
    awaiting_read_.push_back(awaiting_item{std::move(front.sink), front.deadline});
    pending_write_.pop_front();
  }
}

inline auto pipeline::on_message(resp3::message msg) -> void {
  REDISCORO_ASSERT(!awaiting_read_.empty());
  auto& sink = awaiting_read_.front().sink;
  REDISCORO_ASSERT(sink != nullptr);

  sink->deliver(std::move(msg));
  if (sink->is_complete()) {
    awaiting_read_.pop_front();
  }
}

inline auto pipeline::on_error(error_info err) -> void {
  REDISCORO_ASSERT(!awaiting_read_.empty());
  auto& sink = awaiting_read_.front().sink;
  REDISCORO_ASSERT(sink != nullptr);

  sink->deliver_error(std::move(err));
  if (sink->is_complete()) {
    awaiting_read_.pop_front();
  }
}

inline auto pipeline::clear_all(error_info err) -> void {
  // Pending writes: none of the replies will arrive; fail all expected replies.
  while (!pending_write_.empty()) {
    auto& p = pending_write_.front();
    REDISCORO_ASSERT(p.sink != nullptr);
    p.sink->fail_all(err);
    pending_write_.pop_front();
  }

  // Awaiting reads: fail all remaining replies.
  while (!awaiting_read_.empty()) {
    auto& a = awaiting_read_.front();
    REDISCORO_ASSERT(a.sink != nullptr);
    a.sink->fail_all(err);
    awaiting_read_.pop_front();
  }
}

inline auto pipeline::next_deadline() const noexcept -> time_point {
  time_point a = time_point::max();
  time_point b = time_point::max();
  if (!pending_write_.empty()) {
    a = pending_write_.front().deadline;
  }
  if (!awaiting_read_.empty()) {
    b = awaiting_read_.front().deadline;
  }
  return std::min(a, b);
}

inline bool pipeline::has_expired() const noexcept {
  auto now = clock::now();
  const auto d = next_deadline();
  return d != time_point::max() && d <= now;
}

}  // namespace rediscoro::detail
