#pragma once

#include <rediscoro/resp3/parser.hpp>
#include <rediscoro/resp3/type.hpp>

#include <charconv>
#include <cstdlib>
#include <limits>
#include <string_view>

namespace rediscoro::resp3 {

namespace detail {

[[nodiscard]] inline auto find_crlf(std::string_view sv) -> std::size_t {
  return sv.find("\r\n");
}

[[nodiscard]] inline auto parse_i64(std::string_view sv, std::int64_t& out) -> bool {
  if (sv.empty()) {
    return false;
  }
  auto first = sv.data();
  auto last = sv.data() + sv.size();
  auto res = std::from_chars(first, last, out);
  if (res.ec != std::errc{}) {
    return false;
  }
  return res.ptr == last;
}

[[nodiscard]] inline auto parse_double(std::string_view sv, double& out) -> bool {
  if (sv == "inf") {
    out = std::numeric_limits<double>::infinity();
    return true;
  }
  if (sv == "-inf") {
    out = -std::numeric_limits<double>::infinity();
    return true;
  }
  if (sv == "nan") {
    out = std::numeric_limits<double>::quiet_NaN();
    return true;
  }

  // Requires null-terminated input.
  std::string tmp(sv);
  char* end = nullptr;
  out = std::strtod(tmp.c_str(), &end);
  if (end == tmp.c_str()) {
    return false;
  }
  return end == tmp.c_str() + tmp.size();
}

[[nodiscard]] inline auto raw_type_from_code(char c) -> std::optional<raw_type> {
  switch (c) {
    case '+': return raw_type::simple_string;
    case '-': return raw_type::simple_error;
    case ':': return raw_type::integer;
    case ',': return raw_type::double_type;
    case '#': return raw_type::boolean;
    case '(': return raw_type::big_number;
    case '_': return raw_type::null;
    case '$': return raw_type::bulk_string;
    case '!': return raw_type::bulk_error;
    case '=': return raw_type::verbatim_string;
    case '*': return raw_type::array;
    case '%': return raw_type::map;
    case '~': return raw_type::set;
    case '>': return raw_type::push;
    case '|': return raw_type::attribute;
    default: return std::nullopt;
  }
}

}  // namespace detail

auto parser::parse_one(buffer& buf) -> expected<std::uint32_t, error> {
  if (failed_) {
    return unexpected(error::invalid_format);
  }

  if (stack_.empty()) {
    stack_.push_back(frame{.kind = frame_kind::value});
  }

  auto attach_pending_attrs = [&](std::uint32_t node_idx) -> void {
    if (pending_attr_count_ == 0) {
      return;
    }
    auto& n = tree_.nodes.at(node_idx);
    n.first_attr = pending_attr_first_;
    n.attr_count = pending_attr_count_;
    pending_attr_first_ = 0;
    pending_attr_count_ = 0;
  };

  auto push_attr_link = [&](std::uint32_t kv_idx) -> void {
    if (pending_attr_count_ == 0) {
      pending_attr_first_ = static_cast<std::uint32_t>(tree_.links.size());
    }
    tree_.links.push_back(kv_idx);
    pending_attr_count_++;
  };

  auto start_container = [&](raw_type t, std::int64_t len) -> expected<std::optional<std::uint32_t>, error> {
    if (len < -1) {
      failed_ = true;
      return unexpected(error::invalid_length);
    }
    if (len == -1) {
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = raw_type::null});
      attach_pending_attrs(idx);
      return std::optional<std::uint32_t>{idx};
    }

    auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
    tree_.nodes.push_back(raw_node{
      .type = t,
      .text = {},
      .i64 = len,
      .first_child = static_cast<std::uint32_t>(tree_.links.size()),
      .child_count = 0,
    });
    attach_pending_attrs(idx);

    if (len == 0) {
      return std::optional<std::uint32_t>{idx};
    }

    if (t == raw_type::map) {
      stack_.push_back(frame{
        .kind = frame_kind::map_key,
        .container_type = t,
        .expected = len,  // pairs
        .produced = 0,    // pairs produced
        .node_index = idx,
        .first_link = static_cast<std::uint32_t>(tree_.links.size()),
        .pending_key = 0,
        .has_pending_key = false,
      });
    } else {
      stack_.push_back(frame{
        .kind = frame_kind::array,  // used for array/set/push
        .container_type = t,
        .expected = len,       // elements
        .produced = 0,         // elements produced
        .node_index = idx,
        .first_link = static_cast<std::uint32_t>(tree_.links.size()),
        .pending_key = 0,
        .has_pending_key = false,
      });
    }

    return std::optional<std::uint32_t>{};
  };

  auto start_attribute = [&](std::int64_t len) -> expected<std::optional<std::uint32_t>, error> {
    if (len < 0) {
      failed_ = true;
      return unexpected(error::invalid_length);
    }
    if (len == 0) {
      return std::optional<std::uint32_t>{};
    }
    stack_.push_back(frame{
      .kind = frame_kind::attribute,
      .container_type = raw_type::attribute,
      .expected = len,  // pairs
      .produced = 0,    // pairs produced
      .node_index = 0,
      .first_link = static_cast<std::uint32_t>(tree_.links.size()),
      .pending_key = 0,
      .has_pending_key = false,
    });
    return std::optional<std::uint32_t>{};
  };

  auto parse_length_after_type = [&](std::string_view data, std::int64_t& out_len, std::size_t& header_bytes) -> error {
    auto pos = detail::find_crlf(data.substr(1));
    if (pos == std::string_view::npos) {
      return error::needs_more;
    }
    auto len_str = data.substr(1, pos);
    std::int64_t len{};
    if (!detail::parse_i64(len_str, len)) {
      return error::invalid_length;
    }
    out_len = len;
    header_bytes = 1 + pos + 2;
    return error{};
  };

  auto parse_value = [&]() -> expected<std::optional<std::uint32_t>, error> {
    auto data = buf.data();
    if (data.empty()) {
      return unexpected(error::needs_more);
    }

    auto t = data[0];
    auto maybe_rt = detail::raw_type_from_code(t);
    if (!maybe_rt.has_value()) {
      failed_ = true;
      return unexpected(error::invalid_type_byte);
    }

    // Attributes are handled as stack frames, not nodes.
    if (*maybe_rt == raw_type::attribute) {
      std::int64_t len{};
      std::size_t header_bytes{};
      auto e = parse_length_after_type(data, len, header_bytes);
      if (e == error::needs_more) {
        return unexpected(error::needs_more);
      }
      if (e != error{}) {
        failed_ = true;
        return unexpected(e);
      }
      if (len > 0) {
        if (stack_.empty() || stack_.back().kind != frame_kind::value) {
          failed_ = true;
          return unexpected(error::invalid_format);
        }
        stack_.pop_back();
      }
      buf.consume(header_bytes);
      return start_attribute(len);
    }

    // Containers
    if (*maybe_rt == raw_type::array || *maybe_rt == raw_type::map ||
        *maybe_rt == raw_type::set || *maybe_rt == raw_type::push) {
      std::int64_t len{};
      std::size_t header_bytes{};
      auto e = parse_length_after_type(data, len, header_bytes);
      if (e == error::needs_more) {
        return unexpected(error::needs_more);
      }
      if (e != error{}) {
        failed_ = true;
        return unexpected(e);
      }
      if (len > 0) {
        if (stack_.empty() || stack_.back().kind != frame_kind::value) {
          failed_ = true;
          return unexpected(error::invalid_format);
        }
        stack_.pop_back();
      }
      buf.consume(header_bytes);
      return start_container(*maybe_rt, len);
    }

    // Null: "_\r\n"
    if (*maybe_rt == raw_type::null) {
      if (data.size() < 3) {
        return unexpected(error::needs_more);
      }
      if (data[1] != '\r' || data[2] != '\n') {
        failed_ = true;
        return unexpected(error::invalid_format);
      }
      buf.consume(3);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = raw_type::null});
      attach_pending_attrs(idx);
      return std::optional<std::uint32_t>{idx};
    }

    // Boolean: "#t\r\n" / "#f\r\n"
    if (*maybe_rt == raw_type::boolean) {
      if (data.size() < 4) {
        return unexpected(error::needs_more);
      }
      if (data[2] != '\r' || data[3] != '\n') {
        failed_ = true;
        return unexpected(error::invalid_format);
      }
      bool b{};
      if (data[1] == 't') {
        b = true;
      } else if (data[1] == 'f') {
        b = false;
      } else {
        failed_ = true;
        return unexpected(error::invalid_format);
      }
      buf.consume(4);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = raw_type::boolean, .boolean = b});
      attach_pending_attrs(idx);
      return std::optional<std::uint32_t>{idx};
    }

    // Bulk-like: $ ! =
    if (*maybe_rt == raw_type::bulk_string || *maybe_rt == raw_type::bulk_error || *maybe_rt == raw_type::verbatim_string) {
      auto pos = detail::find_crlf(data.substr(1));
      if (pos == std::string_view::npos) {
        return unexpected(error::needs_more);
      }
      std::int64_t len{};
      if (!detail::parse_i64(data.substr(1, pos), len)) {
        failed_ = true;
        return unexpected(error::invalid_length);
      }
      if (len < -1) {
        failed_ = true;
        return unexpected(error::invalid_length);
      }
      auto header_bytes = 1 + pos + 2;
      if (len == -1) {
        buf.consume(header_bytes);
        auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
        tree_.nodes.push_back(raw_node{.type = raw_type::null});
        attach_pending_attrs(idx);
        return std::optional<std::uint32_t>{idx};
      }
      auto need = static_cast<std::size_t>(header_bytes) + static_cast<std::size_t>(len) + 2;
      if (data.size() < need) {
        return unexpected(error::needs_more);
      }
      auto payload = data.substr(header_bytes, static_cast<std::size_t>(len));
      if (data.substr(header_bytes + static_cast<std::size_t>(len), 2) != "\r\n") {
        failed_ = true;
        return unexpected(error::invalid_format);
      }
      buf.consume(need);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = *maybe_rt, .text = payload, .i64 = len});
      attach_pending_attrs(idx);
      return std::optional<std::uint32_t>{idx};
    }

    // Line-like: + - : , (
    auto pos = detail::find_crlf(data.substr(1));
    if (pos == std::string_view::npos) {
      return unexpected(error::needs_more);
    }
    auto line = data.substr(1, pos);
    auto consume_bytes = 1 + pos + 2;

    if (*maybe_rt == raw_type::simple_string || *maybe_rt == raw_type::simple_error || *maybe_rt == raw_type::big_number) {
      buf.consume(consume_bytes);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = *maybe_rt, .text = line});
      attach_pending_attrs(idx);
      return std::optional<std::uint32_t>{idx};
    }

    if (*maybe_rt == raw_type::integer) {
      std::int64_t v{};
      if (!detail::parse_i64(line, v)) {
        failed_ = true;
        return unexpected(error::invalid_integer);
      }
      buf.consume(consume_bytes);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = raw_type::integer, .text = line, .i64 = v});
      attach_pending_attrs(idx);
      return std::optional<std::uint32_t>{idx};
    }

    if (*maybe_rt == raw_type::double_type) {
      double v{};
      if (!detail::parse_double(line, v)) {
        failed_ = true;
        return unexpected(error::invalid_format);
      }
      buf.consume(consume_bytes);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = raw_type::double_type, .text = line, .f64 = v});
      attach_pending_attrs(idx);
      return std::optional<std::uint32_t>{idx};
    }

    failed_ = true;
    return unexpected(error::invalid_format);
  };

  while (true) {
    if (stack_.empty()) {
      return unexpected(error::invalid_format);
    }
    auto& f = stack_.back();
    switch (f.kind) {
      case frame_kind::value: {
        auto r = parse_value();
        if (!r) {
          if (r.error() == error::needs_more) {
            return unexpected(error::needs_more);
          }
          failed_ = true;
          return unexpected(r.error());
        }

        // A value either completed a node, or started a container/attribute (nullopt).
        if (!r->has_value()) {
          // If we just pushed a container/attribute frame, keep driving by pushing a new value frame.
          if (stack_.back().kind != frame_kind::value) {
            stack_.push_back(frame{.kind = frame_kind::value});
          }
          break;
        }

        // Completed a node: pop value frame and attach to parent frames.
        auto child_idx = **r;
        stack_.pop_back();  // pop value frame

        while (true) {
          if (stack_.empty()) {
            return child_idx;
          }
          auto& parent = stack_.back();

          if (parent.kind == frame_kind::array) {
            auto& node = tree_.nodes.at(parent.node_index);
            if (node.child_count == 0) {
              node.first_child = static_cast<std::uint32_t>(tree_.links.size());
            }
            tree_.links.push_back(child_idx);
            parent.produced++;
            node.child_count = parent.produced;
            if (parent.produced == static_cast<std::uint32_t>(parent.expected)) {
              child_idx = parent.node_index;
              stack_.pop_back();
              continue;
            }
            // Need another element
            stack_.push_back(frame{.kind = frame_kind::value});
            break;
          }

          if (parent.kind == frame_kind::map_key) {
            parent.pending_key = child_idx;
            parent.has_pending_key = true;
            parent.kind = frame_kind::map_value;
            stack_.push_back(frame{.kind = frame_kind::value});
            break;
          }

          if (parent.kind == frame_kind::map_value) {
            if (!parent.has_pending_key) {
              failed_ = true;
              return unexpected(error::invalid_format);
            }
            auto& node = tree_.nodes.at(parent.node_index);
            if (node.child_count == 0) {
              node.first_child = static_cast<std::uint32_t>(tree_.links.size());
            }
            tree_.links.push_back(parent.pending_key);
            tree_.links.push_back(child_idx);
            parent.has_pending_key = false;
            parent.produced++;
            node.child_count = parent.produced * 2;
            parent.kind = frame_kind::map_key;
            if (parent.produced == static_cast<std::uint32_t>(parent.expected)) {
              child_idx = parent.node_index;
              stack_.pop_back();
              continue;
            }
            stack_.push_back(frame{.kind = frame_kind::value});
            break;
          }

          if (parent.kind == frame_kind::attribute) {
            if (!parent.has_pending_key) {
              parent.pending_key = child_idx;
              parent.has_pending_key = true;
              stack_.push_back(frame{.kind = frame_kind::value});
              break;
            }

            push_attr_link(parent.pending_key);
            push_attr_link(child_idx);
            parent.has_pending_key = false;
            parent.produced++;
            if (parent.produced == static_cast<std::uint32_t>(parent.expected)) {
              stack_.pop_back();  // pop attribute frame
              // After finishing attributes, continue parsing the next value at the same nesting.
              stack_.push_back(frame{.kind = frame_kind::value});
              break;
            }
            stack_.push_back(frame{.kind = frame_kind::value});
            break;
          }

          // No parent container: child is root.
          return child_idx;
        }
        break;
      }
      case frame_kind::array:
      case frame_kind::map_key:
      case frame_kind::map_value:
      case frame_kind::attribute: {
        // Containers are driven by parsing nested values.
        stack_.push_back(frame{.kind = frame_kind::value});
        break;
      }
    }
  }
}

}  // namespace rediscoro::resp3


