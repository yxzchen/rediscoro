#pragma once

#include <rediscoro/resp3/parser.hpp>
#include <rediscoro/resp3/type.hpp>
#include <rediscoro/assert.hpp>

#include <charconv>
#include <limits>
#include <string_view>

namespace rediscoro::resp3 {

namespace detail {

[[nodiscard]] inline std::size_t find_crlf(std::string_view sv) {
  return sv.find("\r\n");
}

[[nodiscard]] inline bool parse_i64(std::string_view sv, std::int64_t& out) {
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

[[nodiscard]] inline bool parse_double(std::string_view sv, double& out) {
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

  // no allocation: use from_chars for floating-point (C++17+).
  auto first = sv.data();
  auto last = sv.data() + sv.size();
  auto res = std::from_chars(first, last, out, std::chars_format::general);
  if (res.ec != std::errc{}) {
    return false;
  }
  return res.ptr == last;
}

}  // namespace detail

namespace {

enum class value_step : std::uint8_t {
  produced_node,   // node_index is valid
  continue_parsing // started container/attribute OR consumed something and should keep parsing
};

struct value_result {
  value_step step{value_step::continue_parsing};
  std::uint32_t node_index{0};
};

}  // namespace

inline auto parser::parse_one() -> expected<std::uint32_t, rediscoro::error> {
  if (tree_ready_) {
    return unexpected(error::resp3_tree_not_consumed);
  }
  if (failed_) {
    return unexpected(error::resp3_parser_failed);
  }

  if (stack_.empty()) {
    stack_.push_back(frame{.kind = frame_kind::value});
  }

  auto attach_pending_attrs = [&](std::uint32_t node_idx) {
    if (pending_attr_count_ == 0) {
      return;
    }
    auto& n = tree_.nodes.at(node_idx);
    n.first_attr = pending_attr_first_;
    n.attr_count = pending_attr_count_;
    pending_attr_first_ = 0;
    pending_attr_count_ = 0;
  };

  auto push_attr_link = [&](std::uint32_t kv_idx) {
    if (pending_attr_count_ == 0) {
      pending_attr_first_ = static_cast<std::uint32_t>(tree_.links.size());
    }
    tree_.links.push_back(kv_idx);
    pending_attr_count_++;
  };

  auto start_container = [&](type3 t, std::int64_t len) -> expected<std::optional<std::uint32_t>, error> {
    if (len < -1) {
      failed_ = true;
      return unexpected(error::resp3_invalid_length);
    }
    if (len == -1) {
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      // Preserve container type for null container (e.g. *-1, %-1)
      tree_.nodes.push_back(raw_node{.type = t, .i64 = -1});
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

    if (t == type3::map) {
      stack_.push_back(frame{
        .kind = frame_kind::map_key,
        .container_type = t,
        .expected = len,  // pairs
        .produced = 0,    // pairs produced
        .node_index = idx,
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
        .pending_key = 0,
        .has_pending_key = false,
      });
    }

    return std::optional<std::uint32_t>{};
  };

  auto start_attribute = [&](std::int64_t len) -> expected<void, error> {
    if (len < 0) {
      failed_ = true;
      return unexpected(error::resp3_invalid_length);
    }
    if (len == 0) {
      return {};
    }
    stack_.push_back(frame{
      .kind = frame_kind::attribute,
      .container_type = type3::attribute,
      .expected = len,  // pairs
      .produced = 0,    // pairs produced
      .node_index = 0,
      .pending_key = 0,
      .has_pending_key = false,
    });
    return {};
  };

  auto parse_length_after_type = [&](std::string_view data, std::int64_t& out_len, std::size_t& header_bytes) -> error {
    auto pos = detail::find_crlf(data.substr(1));
    if (pos == std::string_view::npos) {
      return error::resp3_needs_more;
    }
    auto len_str = data.substr(1, pos);
    std::int64_t len{};
    if (!detail::parse_i64(len_str, len)) {
      return error::resp3_invalid_length;
    }
    out_len = len;
    header_bytes = 1 + pos + 2;
    return error{};
  };

  auto parse_value = [&]() -> expected<value_result, error> {
    auto data = buf_.data();
    if (data.empty()) {
      return unexpected(error::resp3_needs_more);
    }

    auto t = data[0];
    auto maybe_rt = code_to_type(t);
    if (!maybe_rt.has_value()) {
      failed_ = true;
      return unexpected(error::resp3_invalid_type_byte);
    }

    // Attributes are handled as stack frames, not nodes.
    if (*maybe_rt == type3::attribute) {
      std::int64_t len{};
      std::size_t header_bytes{};
      auto e = parse_length_after_type(data, len, header_bytes);
      if (e == error::resp3_needs_more) {
        return unexpected(error::resp3_needs_more);
      }
      if (e != error{}) {
        failed_ = true;
        return unexpected(e);
      }
      buf_.consume(header_bytes);
      if (len == 0) {
        return value_result{.step = value_step::continue_parsing};
      }
      // Attribute is a prefix modifier for the *next value* in the same value context.
      auto r = start_attribute(len);
      if (!r) {
        return unexpected(r.error());
      }
      return value_result{.step = value_step::continue_parsing};
    }

    // Containers
    if (*maybe_rt == type3::array || *maybe_rt == type3::map ||
        *maybe_rt == type3::set || *maybe_rt == type3::push) {
      std::int64_t len{};
      std::size_t header_bytes{};
      auto e = parse_length_after_type(data, len, header_bytes);
      if (e == error::resp3_needs_more) {
        return unexpected(error::resp3_needs_more);
      }
      if (e != error{}) {
        failed_ = true;
        return unexpected(e);
      }
      if (len > 0 && !stack_.empty() && stack_.back().kind == frame_kind::value) {
        // Replace current value frame with a container frame (if present).
        stack_.pop_back();
      }
      buf_.consume(header_bytes);
      auto started = start_container(*maybe_rt, len);
      if (!started) {
        return unexpected(started.error());
      }
      if (started->has_value()) {
        return value_result{.step = value_step::produced_node, .node_index = **started};
      }
      return value_result{.step = value_step::continue_parsing};
    }

    // Null: "_\r\n"
    if (*maybe_rt == type3::null) {
      if (data.size() < 3) {
        return unexpected(error::resp3_needs_more);
      }
      if (data[1] != '\r' || data[2] != '\n') {
        failed_ = true;
        return unexpected(error::resp3_invalid_null);
      }
      buf_.consume(3);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = type3::null});
      attach_pending_attrs(idx);
      return value_result{.step = value_step::produced_node, .node_index = idx};
    }

    // Boolean: "#t\r\n" / "#f\r\n"
    if (*maybe_rt == type3::boolean) {
      if (data.size() < 4) {
        return unexpected(error::resp3_needs_more);
      }
      if (data[2] != '\r' || data[3] != '\n') {
        failed_ = true;
        return unexpected(error::resp3_invalid_boolean);
      }
      bool b{};
      if (data[1] == 't') {
        b = true;
      } else if (data[1] == 'f') {
        b = false;
      } else {
        failed_ = true;
        return unexpected(error::resp3_invalid_boolean);
      }
      buf_.consume(4);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = type3::boolean, .boolean = b});
      attach_pending_attrs(idx);
      return value_result{.step = value_step::produced_node, .node_index = idx};
    }

    // Bulk-like: $ ! =
    if (*maybe_rt == type3::bulk_string || *maybe_rt == type3::bulk_error || *maybe_rt == type3::verbatim_string) {
      auto pos = detail::find_crlf(data.substr(1));
      if (pos == std::string_view::npos) {
        return unexpected(error::resp3_needs_more);
      }
      std::int64_t len{};
      if (!detail::parse_i64(data.substr(1, pos), len)) {
        failed_ = true;
        return unexpected(error::resp3_invalid_length);
      }
      if (len < -1) {
        failed_ = true;
        return unexpected(error::resp3_invalid_length);
      }
      auto header_bytes = 1 + pos + 2;
      if (len == -1) {
        buf_.consume(header_bytes);
        auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
        // Preserve bulk type for null bulk (e.g. $-1, !-1, =-1)
        tree_.nodes.push_back(raw_node{.type = *maybe_rt, .i64 = -1});
        attach_pending_attrs(idx);
        return value_result{.step = value_step::produced_node, .node_index = idx};
      }
      auto need = static_cast<std::size_t>(header_bytes) + static_cast<std::size_t>(len) + 2;
      if (data.size() < need) {
        return unexpected(error::resp3_needs_more);
      }
      auto payload = data.substr(header_bytes, static_cast<std::size_t>(len));
      if (data.substr(header_bytes + static_cast<std::size_t>(len), 2) != "\r\n") {
        failed_ = true;
        return unexpected(error::resp3_invalid_bulk_trailer);
      }
      buf_.consume(need);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = *maybe_rt, .text = payload, .i64 = len});
      attach_pending_attrs(idx);
      return value_result{.step = value_step::produced_node, .node_index = idx};
    }

    // Line-like: + - : , (
    auto pos = detail::find_crlf(data.substr(1));
    if (pos == std::string_view::npos) {
      return unexpected(error::resp3_needs_more);
    }
    auto line = data.substr(1, pos);
    auto consume_bytes = 1 + pos + 2;

    if (*maybe_rt == type3::simple_string || *maybe_rt == type3::simple_error || *maybe_rt == type3::big_number) {
      buf_.consume(consume_bytes);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = *maybe_rt, .text = line});
      attach_pending_attrs(idx);
      return value_result{.step = value_step::produced_node, .node_index = idx};
    }

    if (*maybe_rt == type3::integer) {
      std::int64_t v{};
      if (!detail::parse_i64(line, v)) {
        failed_ = true;
        return unexpected(error::resp3_invalid_integer);
      }
      buf_.consume(consume_bytes);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = type3::integer, .text = line, .i64 = v});
      attach_pending_attrs(idx);
      return value_result{.step = value_step::produced_node, .node_index = idx};
    }

    if (*maybe_rt == type3::double_type) {
      double v{};
      if (!detail::parse_double(line, v)) {
        failed_ = true;
        return unexpected(error::resp3_invalid_double);
      }
      buf_.consume(consume_bytes);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = type3::double_type, .text = line, .f64 = v});
      attach_pending_attrs(idx);
      return value_result{.step = value_step::produced_node, .node_index = idx};
    }

    failed_ = true;
    return unexpected(error::resp3_invalid_type_byte);
  };

  while (true) {
    if (stack_.empty()) {
      failed_ = true;
      return unexpected(error::resp3_invalid_state);
    }
    auto& f = stack_.back();
    switch (f.kind) {
      case frame_kind::value: {
        auto r = parse_value();
        if (!r) {
          if (r.error() == error::resp3_needs_more) {
            return unexpected(error::resp3_needs_more);
          }
          failed_ = true;
          return unexpected(r.error());
        }

        if (r->step == value_step::continue_parsing) {
          // Do not push value here; let the outer loop decide based on the new top frame,
          // or retry parsing within the same value frame (e.g. |0).
          break;
        }

        // Completed a node: pop value frame and attach to parent frames.
        auto child_idx = r->node_index;
        stack_.pop_back();  // pop value frame

        while (true) {
          if (stack_.empty()) {
            tree_ready_ = true;
            return child_idx;
          }
          auto& parent = stack_.back();

          if (parent.kind == frame_kind::array) {
            REDISCORO_ASSERT(
              parent.container_type == type3::array ||
              parent.container_type == type3::set ||
              parent.container_type == type3::push
            );
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
            REDISCORO_ASSERT(parent.container_type == type3::map);
            parent.pending_key = child_idx;
            parent.has_pending_key = true;
            parent.kind = frame_kind::map_value;
            stack_.push_back(frame{.kind = frame_kind::value});
            break;
          }

          if (parent.kind == frame_kind::map_value) {
            REDISCORO_ASSERT(parent.container_type == type3::map);
            if (!parent.has_pending_key) {
              failed_ = true;
              return unexpected(error::resp3_invalid_map_pairs);
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
            REDISCORO_ASSERT(parent.container_type == type3::attribute);
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
              // Attribute is a prefix modifier; do not push value here.
              break;
            }
            stack_.push_back(frame{.kind = frame_kind::value});
            break;
          }

          // No parent container: child is root.
          tree_ready_ = true;
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

inline auto parser::reclaim() -> void {
  // After the user has consumed the raw tree, it is now safe to reclaim memory.
  tree_.reset();
  stack_.clear();
  pending_attr_first_ = 0;
  pending_attr_count_ = 0;
  tree_ready_ = false;
  buf_.compact();
}

}  // namespace rediscoro::resp3


