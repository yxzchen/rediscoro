#pragma once

#include <rediscoro/detail/pipeline.hpp>

namespace rediscoro::detail {

inline auto pipeline::push(request req, response_sink* sink) -> void {
  // TODO: Implementation
  // - Add request to pending_write_ queue
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
  // TODO: Implementation
  // - Return view into next request's wire bytes
  // - Account for bytes already written
  return {};
}

inline auto pipeline::on_write_done(std::size_t n) -> void {
  // TODO: Implementation
  // - Update bytes written for front request
  // - If request fully written, move to awaiting_read_
}

inline auto pipeline::on_message(resp3::message msg) -> void {
  // TODO: Implementation
  // - Pop front from awaiting_read_
  // - Adapt message to expected type
  // - Set value/error on pending_response
}

inline auto pipeline::on_error(resp3::error err) -> void {
  // TODO: Implementation
  // - Pop front from awaiting_read_
  // - Set error on pending_response
}

inline auto pipeline::clear_all(response_error err) -> void {
  // TODO: Implementation
  // - Set error on all pending responses
  // - Clear all queues
}

}  // namespace rediscoro::detail
