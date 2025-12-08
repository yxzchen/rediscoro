/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#include <xz/redis/assert.hpp>
#include <xz/redis/error.hpp>
#include <xz/redis/resp3/parser.hpp>

#include <charconv>
#include <cstddef>
#include <limits>
#include <vector>

namespace xz::redis::resp3 {

void to_int(std::size_t& i, std::string_view sv, std::error_code& ec) {
  auto const res = std::from_chars(sv.data(), sv.data() + sv.size(), i);
  if (res.ec != std::errc()) {
    ec = error::not_a_number;
  } else if (res.ptr != sv.data() + sv.size()) {
    ec = error::invalid_number_format;
  }
}

// Cascades element completion upward through the stack
// Example: in array of size 2, completing the 2nd element completes the array too
void parser::commit_elem() noexcept {
  REDISUS_ASSERT(!pending_.empty());
  if (pending_.empty()) return;

  pending_.top()--;
  while (pending_.top() == 0) {
    pending_.pop();
    if (pending_.empty()) break;
    pending_.top()--;
  }
}

auto parser::read_until_separator() noexcept -> std::optional<std::string_view> {
  auto view = buffer_.view();
  auto const pos = view.find(sep);
  if (pos == std::string::npos) {
    return std::nullopt;
  }

  auto const result = view.substr(0, pos);
  buffer_.consume(pos + 2);
  return result;
}

auto parser::read_bulk_data(std::size_t length, std::error_code& ec) noexcept -> std::optional<std::string_view> {
  auto view = buffer_.view();

  // Check for overflow when adding 2 for \r\n
  if (length > std::numeric_limits<std::size_t>::max() - 2) {
    ec = error::invalid_data_type;
    return std::nullopt;
  }

  auto const span = length + 2;
  if (view.length() < span) {
    return std::nullopt;  // Need more data
  }

  // Validate CRLF terminator
  if (view[length] != '\r' || view[length + 1] != '\n') {
    ec = error::invalid_data_type;
    return std::nullopt;
  }

  auto const result = view.substr(0, length);
  buffer_.consume(span);
  return result;
}

auto parser::parse() -> generator<std::optional<msg_view>> {
  msg_view msg;
  msg.reserve(16);  // Reserve initial capacity to reduce reallocations
  std::optional<std::string_view> line;
  std::optional<std::string_view> bulk_data;

  while (!ec_) {
    // === Prepare for new message ===
    if (pending_.empty()) {
      pending_.push(1);
    }

    // === Wait for header line ===
    while (!(line = read_until_separator())) {
      co_yield std::nullopt;
    }

    // === Parse header ===
    REDISUS_ASSERT(!pending_.empty());
    if (line->empty()) {
      ec_ = error::invalid_data_type;
      co_return;
    }

    auto type = to_type(line->at(0));
    auto elem = line->substr(1);

    // === Parse element by type ===
    switch (type) {
      case type3::streamed_string_part: {
        // Zero-length part ends the stream by setting pending to 1
        std::size_t bulk_length;
        to_int(bulk_length, elem, ec_);
        if (ec_) co_return;

        if (bulk_length == 0) {
          pending_.top() = 1;
          commit_elem();
          msg.push_back(node_view{type3::streamed_string_part, std::string_view{}});
        } else {
          while (!(bulk_data = read_bulk_data(bulk_length, ec_))) {
            if (ec_) co_return;
            co_yield std::nullopt;
          }
          commit_elem();
          msg.push_back(node_view{type3::streamed_string_part, *bulk_data});
        }
        break;
      }

      case type3::blob_error:
      case type3::verbatim_string:
      case type3::blob_string: {
        if (elem.empty()) {
          ec_ = error::empty_field;
          co_return;
        }

        if (elem.at(0) == '?') {
          // Streamed string: push max size_t as sentinel, consumed by string parts
          if (pending_.size() >= max_depth_) {
            ec_ = error::exceeeds_max_nested_depth;
            co_return;
          }
          pending_.push(std::numeric_limits<std::size_t>::max());
          msg.push_back(node_view{type3::streamed_string, std::size_t{0}});
        } else {
          std::size_t bulk_length;
          to_int(bulk_length, elem, ec_);
          if (ec_) co_return;

          while (!(bulk_data = read_bulk_data(bulk_length, ec_))) {
            if (ec_) co_return;
            co_yield std::nullopt;
          }
          commit_elem();
          msg.push_back(node_view{type, *bulk_data});
        }
        break;
      }

      case type3::boolean: {
        if (elem.empty()) {
          ec_ = error::empty_field;
          co_return;
        }

        if (elem.at(0) != 'f' && elem.at(0) != 't') {
          ec_ = error::unexpected_bool_value;
          co_return;
        }

        commit_elem();
        msg.push_back(node_view{type, elem});
        break;
      }

      case type3::doublean:
      case type3::big_number:
      case type3::number: {
        if (elem.empty()) {
          ec_ = error::empty_field;
          co_return;
        }
        [[fallthrough]];
      }

      case type3::simple_error:
      case type3::simple_string:
      case type3::null: {
        commit_elem();
        msg.push_back(node_view{type, elem});
        break;
      }

      case type3::push:
      case type3::set:
      case type3::array:
      case type3::attribute:
      case type3::map: {
        std::size_t size;
        to_int(size, elem, ec_);
        if (ec_) co_return;

        if (size == 0) {
          commit_elem();
        } else {
          if (pending_.size() >= max_depth_) {
            ec_ = error::exceeeds_max_nested_depth;
            co_return;
          }

          auto const multiplicity = element_multiplicity(type);
          if (size > std::numeric_limits<std::size_t>::max() / multiplicity) {
            ec_ = error::aggregate_size_overflow;
            co_return;
          }

          pending_.push(size * multiplicity);
        }

        msg.push_back(node_view{type, size});
        break;
      }

      default: {
        ec_ = error::invalid_data_type;
        co_return;
      }
    }

    // === Yield complete message ===
    if (pending_.empty()) {
      co_yield std::optional<msg_view>{std::move(msg)};
      msg.clear();
    }
  }
}

}  // namespace xz::redis::resp3
