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
#include <vector>

namespace redisus::resp3 {

void to_int(std::size_t& i, std::string_view sv, std::error_code& ec) {
  auto const res = std::from_chars(sv.data(), sv.data() + std::size(sv), i);
  if (res.ec != std::errc()) ec = error::not_a_number;
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

auto parser::parse() -> generator<std::optional<std::vector<node_view>>> {
  std::vector<node_view> nodes;
  std::optional<std::string_view> line;
  std::optional<std::string_view> bulk_data;

  while (!ec_) {
    // === Prepare for new message ===
    if (pending_.empty()) {
      pending_.push(1);
    }

    // === Wait for header line ===
    while (buffer_.empty()) {
      co_yield std::nullopt;
    }

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
        std::size_t bulk_length;
        to_int(bulk_length, elem, ec_);
        if (ec_) co_return;

        if (bulk_length == 0) {
          pending_.top() = 1;
          commit_elem();
          nodes.push_back(node_view{type3::streamed_string_part, std::string_view{}});
        } else {
          while (!(bulk_data = read_bulk_data(bulk_length))) {
            co_yield std::nullopt;
          }
          commit_elem();
          nodes.push_back(node_view{type3::streamed_string_part, *bulk_data});
        }
        break;
      }

      case type3::blob_error:
      case type3::verbatim_string:
      case type3::blob_string: {
        if (std::empty(elem)) {
          ec_ = error::empty_field;
          co_return;
        }

        if (elem.at(0) == '?') {
          pending_.push(std::numeric_limits<std::size_t>::max());
          nodes.push_back(node_view{type3::streamed_string, std::size_t{0}});
        } else {
          std::size_t bulk_length;
          to_int(bulk_length, elem, ec_);
          if (ec_) co_return;

          while (!(bulk_data = read_bulk_data(bulk_length))) {
            co_yield std::nullopt;
          }
          commit_elem();
          nodes.push_back(node_view{type, *bulk_data});
        }
        break;
      }

      case type3::boolean: {
        if (std::empty(elem)) {
          ec_ = error::empty_field;
          co_return;
        }

        if (elem.at(0) != 'f' && elem.at(0) != 't') {
          ec_ = error::unexpected_bool_value;
          co_return;
        }

        commit_elem();
        nodes.push_back(node_view{type, elem});
        break;
      }

      case type3::doublean:
      case type3::big_number:
      case type3::number: {
        if (std::empty(elem)) {
          ec_ = error::empty_field;
          co_return;
        }
        [[fallthrough]];
      }

      case type3::simple_error:
      case type3::simple_string:
      case type3::null: {
        commit_elem();
        nodes.push_back(node_view{type, elem});
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
          pending_.push(size * element_multiplicity(type));
        }

        nodes.push_back(node_view{type, size});
        break;
      }

      default: {
        ec_ = error::invalid_data_type;
        co_return;
      }
    }

    // === Yield complete message ===
    if (pending_.empty()) {
      co_yield std::optional<std::vector<node_view>>{std::move(nodes)};
      nodes.clear();
    }
  }
}

}  // namespace redisus::resp3
