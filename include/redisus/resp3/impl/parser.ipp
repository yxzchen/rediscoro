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

void to_int(std::size_t& i, std::string_view sv, std::error_code& ec) {
  auto const res = std::from_chars(sv.data(), sv.data() + std::size(sv), i);
  if (res.ec != std::errc()) ec = error::not_a_number;
}

std::size_t parser::consumed() const noexcept {
  return consumed_;
}

bool parser::done() const noexcept {
  return remaining_.empty();
}

void parser::commit_elem() noexcept {
  remaining_.top()--;
  while (remaining_.top() == 0) {
    remaining_.pop();

    if (remaining_.empty()) break;
    remaining_.top()--;
  }
}

auto parser::consume(std::string_view view, std::error_code& ec) noexcept -> parser::result {
  switch (bulk_type_) {
    case type_t::invalid: {
      auto const pos = view.find(sep, consumed_);
      if (pos == std::string::npos) return std::nullopt;  // Needs more data to proceeed.

      auto const type = to_type(view.at(consumed_));
      auto const elem = view.substr(consumed_ + 1, pos - 1 - consumed_);
      auto const ret = consume_impl(type, elem, ec);
      if (ec) return std::nullopt;

      consumed_ = pos + 2;

      if (ret) return ret;
    }
      [[fallthrough]];

    default: {
      auto const span = bulk_length_ + 2;
      if (view.length() - consumed_ < span) return std::nullopt;  // Needs more data to proceeed.

      auto const bulk_view = view.substr(consumed_, bulk_length_);
      node_view const ret = {bulk_type_, bulk_view};
      bulk_type_ = type_t::invalid;
      commit_elem();

      consumed_ += span;
      return ret;
    }
  }
}

auto parser::consume_impl(type_t type, std::string_view elem, std::error_code& ec) -> parser::result {
  REDISUS_ASSERT(bulk_type_ == type_t::invalid);

  node_view ret;
  switch (type) {
    case type_t::streamed_string_part: {
      REDISUS_ASSERT(!remaining_.empty());

      to_int(bulk_length_, elem, ec);
      if (ec) return std::nullopt;

      if (bulk_length_ == 0) {
        ret = {type_t::streamed_string_part, std::string_view{}};
        remaining_.top() = 1;
        commit_elem();
      } else {
        bulk_type_ = type_t::streamed_string_part;
        return std::nullopt;
      }
      break;
    }
    case type_t::blob_error:
    case type_t::verbatim_string:
    case type_t::blob_string: {
      if (std::empty(elem)) {
        ec = error::empty_field;
        return std::nullopt;
      }
      if (elem.at(0) == '?') {
        // NOTE: This can only be triggered with blob_string.
        // Trick: A streamed string is read as an aggregate of
        // infinite length. When the streaming is done the server
        // is supposed to send a part with length 0.
        remaining_.push(std::numeric_limits<std::size_t>::max());
        ret = {type_t::streamed_string, std::size_t{0}};
      } else {
        to_int(bulk_length_, elem, ec);
        if (ec) return std::nullopt;

        bulk_type_ = type;
        return std::nullopt;
      }
      break;
    }

    case type_t::boolean: {
      if (std::empty(elem)) {
        ec = error::empty_field;
        return std::nullopt;
      }

      if (elem.at(0) != 'f' && elem.at(0) != 't') {
        ec = error::unexpected_bool_value;
        return std::nullopt;
      }

      ret = {type, elem};
      commit_elem();
      break;
    }
    case type_t::doublean:
    case type_t::big_number:
    case type_t::number: {
      if (std::empty(elem)) {
        ec = error::empty_field;
        return std::nullopt;
      }
    }
      [[fallthrough]];
    case type_t::simple_error:
    case type_t::simple_string:
    case type_t::null: {
      ret = {type, elem};
      commit_elem();
      break;
    }
    case type_t::push:
    case type_t::set:
    case type_t::array:
    case type_t::attribute:
    case type_t::map: {
      std::size_t size;
      to_int(size, elem, ec);
      if (ec) return std::nullopt;

      ret = {type, size};
      if (size == 0) {
        commit_elem();
      } else {
        remaining_.push(size * element_multiplicity(type));
      }
      break;
    }
    default: {
      ec = error::invalid_data_type;
      return std::nullopt;
    }
  }

  return ret;
}

}  // namespace redisus::resp3
