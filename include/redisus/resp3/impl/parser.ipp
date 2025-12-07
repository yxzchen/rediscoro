/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#include <redisus/assert.hpp>
#include <redisus/error.hpp>
#include <redisus/resp3/parser.hpp>

#include <charconv>
#include <cstddef>
#include <limits>

namespace redisus::resp3 {

namespace {

void to_int(std::size_t& i, std::string_view sv, std::error_code& ec) {
  auto const res = std::from_chars(sv.data(), sv.data() + std::size(sv), i);
  if (res.ec != std::errc()) ec = error::not_a_number;
}

}  // namespace

bool parser::done() const noexcept {
  return pending_.empty();
}

void parser::commit_elem() noexcept {
  pending_.top()--;
  while (pending_.top() == 0) {
    pending_.pop();

    if (pending_.empty()) break;
    pending_.top()--;
  }
}

auto parser::read_until_separator() -> std::optional<std::string_view> {
  auto view = buffer_.view();
  auto const pos = view.find(sep);
  if (pos == std::string::npos) {
    return std::nullopt;  // Need more data
  }

  auto const result = view.substr(0, pos);
  buffer_.consume(pos + 2);  // Consume including \r\n
  return result;
}

auto parser::read_bulk_data(std::size_t length) -> std::optional<std::string_view> {
  auto view = buffer_.view();
  auto const span = length + 2;  // Include \r\n
  if (view.length() < span) {
    return std::nullopt;  // Need more data
  }

  auto const result = view.substr(0, length);
  buffer_.consume(span);  // Consume including \r\n
  return result;
}

auto parser::parse(std::error_code& ec) -> generator<std::optional<node_view>> {
  // Parse forever until error
  while (!ec) {
    switch (state_) {
      case state::read_header: {
        // Wait for data if buffer is empty
        if (buffer_.empty()) {
          co_yield std::nullopt;
          continue;
        }

        // Read line until \r\n
        auto line = read_until_separator();
        if (!line) {
          co_yield std::nullopt;
          continue;
        }

        REDISUS_ASSERT(!pending_.empty());

        if (line->empty()) {
          ec = error::invalid_data_type;
          co_return;
        }

        // Extract type from first character
        char type_char = line->at(0);
        type3 type = to_type(type_char);

        // Rest is element data
        auto elem = line->substr(1);

        switch (type) {
          case type3::streamed_string_part: {
            std::size_t bulk_length;
            to_int(bulk_length, elem, ec);
            if (ec) co_return;

            if (bulk_length == 0) {
              // Terminator for streamed string
              pending_.top() = 1;
              commit_elem();
              co_yield std::optional<node_view>{node_view{type3::streamed_string_part, std::string_view{}}};
            } else {
              // Read the bulk data
              auto bulk_data = read_bulk_data(bulk_length);
              if (!bulk_data) {
                // Need more data - save state and yield
                pending_bulk_length_ = bulk_length;
                pending_type_ = type3::streamed_string_part;
                state_ = state::read_bulk_data;
                co_yield std::nullopt;
                continue;
              }

              commit_elem();
              co_yield std::optional<node_view>{node_view{type3::streamed_string_part, *bulk_data}};
            }
            break;
          }

          case type3::blob_error:
          case type3::verbatim_string:
          case type3::blob_string: {
            if (std::empty(elem)) {
              ec = error::empty_field;
              co_return;
            }

            if (elem.at(0) == '?') {
              // Streamed string marker
              pending_.push(std::numeric_limits<std::size_t>::max());
              co_yield std::optional<node_view>{node_view{type3::streamed_string, std::size_t{0}}};
            } else {
              std::size_t bulk_length;
              to_int(bulk_length, elem, ec);
              if (ec) co_return;

              // Read the bulk data
              auto bulk_data = read_bulk_data(bulk_length);
              if (!bulk_data) {
                // Need more data - save state and yield
                pending_bulk_length_ = bulk_length;
                pending_type_ = type;
                state_ = state::read_bulk_data;
                co_yield std::nullopt;
                continue;
              }

              commit_elem();
              co_yield std::optional<node_view>{node_view{type, *bulk_data}};
            }
            break;
          }

          case type3::boolean: {
            if (std::empty(elem)) {
              ec = error::empty_field;
              co_return;
            }

            if (elem.at(0) != 'f' && elem.at(0) != 't') {
              ec = error::unexpected_bool_value;
              co_return;
            }

            commit_elem();
            co_yield std::optional<node_view>{node_view{type, elem}};
            break;
          }

          case type3::doublean:
          case type3::big_number:
          case type3::number: {
            if (std::empty(elem)) {
              ec = error::empty_field;
              co_return;
            }
            [[fallthrough]];
          }

          case type3::simple_error:
          case type3::simple_string:
          case type3::null: {
            commit_elem();
            co_yield std::optional<node_view>{node_view{type, elem}};
            break;
          }

          case type3::push:
          case type3::set:
          case type3::array:
          case type3::attribute:
          case type3::map: {
            std::size_t size;
            to_int(size, elem, ec);
            if (ec) co_return;

            if (size == 0) {
              commit_elem();
            } else {
              pending_.push(size * element_multiplicity(type));
            }

            co_yield std::optional<node_view>{node_view{type, size}};
            break;
          }

          default: {
            ec = error::invalid_data_type;
            co_return;
          }
        }
        break;
      }

      case state::read_bulk_data: {
        // Resume reading bulk data
        auto bulk_data = read_bulk_data(pending_bulk_length_);
        if (!bulk_data) {
          co_yield std::nullopt;
          continue;
        }

        commit_elem();
        co_yield std::optional<node_view>{node_view{pending_type_, *bulk_data}};

        // Reset to header state
        state_ = state::read_header;
        pending_bulk_length_ = 0;
        pending_type_ = type3::invalid;
        break;
      }
    }
  }
}

}  // namespace redisus::resp3
