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
  return remaining_.empty() && next_type_ == type_t::invalid && consumed_ != 0;
}

void parser::commit_elem() noexcept {
  remaining_.top()--;
  while (remaining_.top() == 0) {
    remaining_.pop();
    remaining_.top()--;
  }
}

auto parser::consume(std::string_view view, std::error_code& ec) noexcept -> parser::result {
  switch (next_type_) {
    case type_t::invalid: {
      remaining_.push(1);

      auto const pos = view.find(sep, consumed_);
      if (pos == std::string::npos) return {};  // Needs more data to proceeed.

      auto const type = to_type(view.at(consumed_));
      auto const elem = view.substr(consumed_ + 1, pos - 1 - consumed_);
      auto const ret = consume_impl(type, elem, ec);
      if (ec) return {};

      consumed_ = pos + 2;

      if (ret) return ret;
    }
      [[fallthrough]];

    default: {
      auto const span = next_length_ + 2;
      if ((std::size(view) - consumed_) < span) return {};  // Needs more data to proceeed.

      auto const bulk_view = view.substr(consumed_, next_length_);
      node_type const ret = {next_type_, 1, bulk_view};
      next_type_ = type_t::invalid;
      commit_elem();

      consumed_ += span;
      return ret;
    }
  }
}

auto parser::consume_impl(type_t type, std::string_view elem, std::error_code& ec) -> parser::result {
  node_type ret;
  switch (type) {
    case type_t::streamed_string_part: {
      REDISUS_ASSERT(next_type_ == type_t::streamed_string_part);

      to_int(next_length_, elem, ec);
      if (ec) return {};

      if (next_length_ == 0) {
        ret = {type_t::streamed_string_part, 1, {}};
        remaining_.top() = 1;
        commit_elem();
        next_type_ = type_t::invalid;
      }
      break;
    }
    case type_t::blob_error:
    case type_t::verbatim_string:
    case type_t::blob_string: {
      if (elem.at(0) == '?') {
        // NOTE: This can only be triggered with blob_string.
        // Trick: A streamed string is read as an aggregate of
        // infinite length. When the streaming is done the server
        // is supposed to send a part with length 0.
        remaining_.push(std::numeric_limits<std::size_t>::max());
        ret = {type_t::streamed_string, 0, {}};
        next_type_ = type_t::streamed_string_part;
      } else {
        to_int(next_length_, elem, ec);
        if (ec) return {};

        next_type_ = type;
      }
      break;
    }

    case type_t::boolean: {
      if (std::empty(elem)) {
        ec = error::empty_field;
        return {};
      }

      if (elem.at(0) != 'f' && elem.at(0) != 't') {
        ec = error::unexpected_bool_value;
        return {};
      }

      ret = {type, 1, elem};
      commit_elem();
      break;
    }
    case type_t::doublean:
    case type_t::big_number:
    case type_t::number: {
      if (std::empty(elem)) {
        ec = error::empty_field;
        return {};
      }
    }
      [[fallthrough]];
    case type_t::simple_error:
    case type_t::simple_string:
    case type_t::null: {
      ret = {type, 1, elem};
      commit_elem();
      break;
    }
    case type_t::push:
    case type_t::set:
    case type_t::array:
    case type_t::attribute:
    case type_t::map: {
      std::size_t l = static_cast<std::size_t>(-1);
      to_int(l, elem, ec);
      if (ec) return {};

      ret = {type, l, {}};
      if (l == 0) {
        commit_elem();
      } else {
        remaining_.push(l * element_multiplicity(type));
        next_type_ = type;
      }
      break;
    }
    default: {
      ec = error::invalid_data_type;
      return {};
    }
  }

  return ret;
}

}  // namespace redisus::resp3
