#pragma once

#include <rediscoro/detail/pipeline.hpp>

namespace rediscoro::detail {

inline auto pipeline::push(request req, response_sink* sink) -> void {
  REDISCORO_ASSERT(sink != nullptr);
  REDISCORO_ASSERT(req.reply_count() == sink->expected_replies());

  pending_write_.push_back(pending_item{std::move(req), sink, 0});
}

inline auto pipeline::has_pending_write() const noexcept -> bool {
  // TODO: Implementation
  return !pending_write_.empty();
}

inline auto pipeline::has_pending_read() const noexcept -> bool {
  // TODO: Implementation
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
    // Entire request written: move to awaiting read queue with reply count.
    awaiting_read_.push_back(awaiting_item{front.sink, front.req.reply_count()});
    pending_write_.pop_front();
  }
}

inline auto pipeline::on_message(resp3::message msg) -> void {
  REDISCORO_ASSERT(!awaiting_read_.empty());
  auto& a = awaiting_read_.front();
  REDISCORO_ASSERT(a.sink != nullptr);
  REDISCORO_ASSERT(a.remaining > 0);

  a.sink->deliver(std::move(msg));
  a.remaining -= 1;
  if (a.remaining == 0) {
    awaiting_read_.pop_front();
  }
}

inline auto pipeline::on_error(resp3::error err) -> void {
  REDISCORO_ASSERT(!awaiting_read_.empty());
  auto& a = awaiting_read_.front();
  REDISCORO_ASSERT(a.sink != nullptr);
  REDISCORO_ASSERT(a.remaining > 0);

  a.sink->deliver_error(response_error{err});
  a.remaining -= 1;
  if (a.remaining == 0) {
    awaiting_read_.pop_front();
  }
}

inline auto pipeline::clear_all(response_error err) -> void {
  // Pending writes: none of the replies will arrive; fail all expected replies.
  for (auto& p : pending_write_) {
    REDISCORO_ASSERT(p.sink != nullptr);
    const auto replies = p.req.reply_count();
    for (std::size_t i = 0; i < replies; ++i) {
      p.sink->deliver_error(err);
    }
  }
  pending_write_.clear();

  // Awaiting reads: fail all remaining replies.
  for (auto& a : awaiting_read_) {
    REDISCORO_ASSERT(a.sink != nullptr);
    for (std::size_t i = 0; i < a.remaining; ++i) {
      a.sink->deliver_error(err);
    }
  }
  awaiting_read_.clear();
}

}  // namespace rediscoro::detail
