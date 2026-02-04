#pragma once

#include <rediscoro/assert.hpp>
#include <rediscoro/resp3/kind.hpp>
#include <rediscoro/resp3/parser.hpp>

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

inline auto parser::parse_length_after_type(std::string_view data) const
  -> expected<std::optional<length_header>, rediscoro::protocol_errc> {
  auto pos = detail::find_crlf(data.substr(1));
  if (pos == std::string_view::npos) {
    return std::optional<length_header>{};
  }

  auto len_str = data.substr(1, pos);
  std::int64_t len{};
  if (!detail::parse_i64(len_str, len)) {
    return unexpected(protocol_errc::invalid_length);
  }

  return std::optional<length_header>{length_header{
    .length = len,
    .header_bytes = 1 + pos + 2,
  }};
}

inline auto parser::start_container(frame& current, kind t, std::int64_t len)
  -> expected<container_result, rediscoro::protocol_errc> {
  if (len < -1) {
    return unexpected(protocol_errc::invalid_length);
  }

  if (len == -1) {
    auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
    // Preserve container type for null container (e.g. *-1, %-1)
    tree_.nodes.push_back(raw_node{.type = t, .i64 = -1});
    pending_attrs_.attach(tree_, idx);
    return container_result{.step = container_step::produced_node, .node_index = idx};
  }

  auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
  tree_.nodes.push_back(raw_node{
    .type = t,
    .text = {},
    .i64 = len,
    .first_child = static_cast<std::uint32_t>(tree_.links.size()),
    .child_count = 0,
  });
  pending_attrs_.attach(tree_, idx);

  if (len == 0) {
    return container_result{.step = container_step::produced_node, .node_index = idx};
  }

  // Replace current value frame with a container-driving frame.
  if (t == kind::map) {
    current = frame{
      .state = frame_kind::map_key,
      .container_type = t,
      .expected = len,  // pairs
      .produced = 0,    // pairs produced
      .node_index = idx,
      .pending_key = 0,
      .has_pending_key = false,
    };
  } else {
    current = frame{
      .state = frame_kind::array,  // used for array/set/push
      .container_type = t,
      .expected = len,  // elements
      .produced = 0,    // elements produced
      .node_index = idx,
      .pending_key = 0,
      .has_pending_key = false,
    };
  }

  return container_result{.step = container_step::started_container};
}

inline auto parser::start_attribute(std::int64_t len)
  -> expected<std::monostate, rediscoro::protocol_errc> {
  if (len < 0) {
    return unexpected(protocol_errc::invalid_length);
  }
  if (len == 0) {
    return std::monostate{};
  }

  stack_.push_back(frame{
    .state = frame_kind::attribute,
    .container_type = kind::attribute,
    .expected = len,  // pairs
    .produced = 0,    // pairs produced
    .node_index = 0,
    .pending_key = 0,
    .has_pending_key = false,
  });
  return std::monostate{};
}

inline auto parser::parse_value()
  -> expected<std::optional<value_result>, rediscoro::protocol_errc> {
  REDISCORO_ASSERT(!stack_.empty());
  REDISCORO_ASSERT(stack_.back().state == frame_kind::value);

  auto data = buf_.data();
  if (data.empty()) {
    return std::optional<value_result>{};
  }

  auto maybe_t = prefix_to_kind(data[0]);
  if (!maybe_t.has_value()) {
    return unexpected(protocol_errc::invalid_type_byte);
  }

  // Attribute prefix: handled as a frame, not a node.
  if (*maybe_t == kind::attribute) {
    auto hdr = parse_length_after_type(data);
    if (!hdr) {
      return unexpected(hdr.error());
    }
    if (!hdr->has_value()) {
      return std::optional<value_result>{};
    }
    auto const& lh = **hdr;
    buf_.consume(lh.header_bytes);
    auto started = start_attribute(lh.length);
    if (!started) {
      return unexpected(started.error());
    }
    return std::optional<value_result>{value_result{.step = value_step::continue_parsing}};
  }

  // Containers: array/map/set/push
  if (*maybe_t == kind::array || *maybe_t == kind::map || *maybe_t == kind::set ||
      *maybe_t == kind::push) {
    auto hdr = parse_length_after_type(data);
    if (!hdr) {
      return unexpected(hdr.error());
    }
    if (!hdr->has_value()) {
      return std::optional<value_result>{};
    }
    auto const& lh = **hdr;
    buf_.consume(lh.header_bytes);

    auto& current = stack_.back();
    auto started = start_container(current, *maybe_t, lh.length);
    if (!started) {
      return unexpected(started.error());
    }
    if (started->step == container_step::produced_node) {
      return std::optional<value_result>{
        value_result{.step = value_step::produced_node, .node_index = started->node_index}
      };
    }
    return std::optional<value_result>{value_result{.step = value_step::continue_parsing}};
  }

  // Null: "_\r\n"
  if (*maybe_t == kind::null) {
    if (data.size() < 3) {
      return std::optional<value_result>{};
    }
    if (data[1] != '\r' || data[2] != '\n') {
      return unexpected(protocol_errc::invalid_null);
    }
    buf_.consume(3);
    auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
    tree_.nodes.push_back(raw_node{.type = kind::null});
    pending_attrs_.attach(tree_, idx);
    return std::optional<value_result>{value_result{.step = value_step::produced_node, .node_index = idx}};
  }

  // Boolean: "#t\r\n" / "#f\r\n"
  if (*maybe_t == kind::boolean) {
    if (data.size() < 4) {
      return std::optional<value_result>{};
    }
    if (data[2] != '\r' || data[3] != '\n') {
      return unexpected(protocol_errc::invalid_boolean);
    }

    bool b{};
    if (data[1] == 't') {
      b = true;
    } else if (data[1] == 'f') {
      b = false;
    } else {
      return unexpected(protocol_errc::invalid_boolean);
    }

    buf_.consume(4);
    auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
    tree_.nodes.push_back(raw_node{.type = kind::boolean, .boolean = b});
    pending_attrs_.attach(tree_, idx);
    return std::optional<value_result>{value_result{.step = value_step::produced_node, .node_index = idx}};
  }

  // Bulk-like: $ ! =
  if (*maybe_t == kind::bulk_string || *maybe_t == kind::bulk_error ||
      *maybe_t == kind::verbatim_string) {
    auto pos = detail::find_crlf(data.substr(1));
    if (pos == std::string_view::npos) {
      return std::optional<value_result>{};
    }
    std::int64_t len{};
    if (!detail::parse_i64(data.substr(1, pos), len)) {
      return unexpected(protocol_errc::invalid_length);
    }
    if (len < -1) {
      return unexpected(protocol_errc::invalid_length);
    }

    auto header_bytes = 1 + pos + 2;
    if (len == -1) {
      buf_.consume(header_bytes);
      auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
      tree_.nodes.push_back(raw_node{.type = *maybe_t, .i64 = -1});
      pending_attrs_.attach(tree_, idx);
      return std::optional<value_result>{
        value_result{.step = value_step::produced_node, .node_index = idx}
      };
    }

    const auto need = static_cast<std::size_t>(header_bytes) + static_cast<std::size_t>(len) + 2;
    if (data.size() < need) {
      return std::optional<value_result>{};
    }

    auto payload = data.substr(header_bytes, static_cast<std::size_t>(len));
    if (data.substr(header_bytes + static_cast<std::size_t>(len), 2) != "\r\n") {
      return unexpected(protocol_errc::invalid_bulk_trailer);
    }

    buf_.consume(need);
    auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
    tree_.nodes.push_back(raw_node{.type = *maybe_t, .text = payload, .i64 = len});
    pending_attrs_.attach(tree_, idx);
    return std::optional<value_result>{
      value_result{.step = value_step::produced_node, .node_index = idx}
    };
  }

  // Line-like: + - : , (
  auto pos = detail::find_crlf(data.substr(1));
  if (pos == std::string_view::npos) {
    return std::optional<value_result>{};
  }
  auto line = data.substr(1, pos);
  auto consume_bytes = 1 + pos + 2;

  if (*maybe_t == kind::simple_string || *maybe_t == kind::simple_error ||
      *maybe_t == kind::big_number) {
    buf_.consume(consume_bytes);
    auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
    tree_.nodes.push_back(raw_node{.type = *maybe_t, .text = line});
    pending_attrs_.attach(tree_, idx);
    return std::optional<value_result>{value_result{.step = value_step::produced_node, .node_index = idx}};
  }

  if (*maybe_t == kind::integer) {
    std::int64_t v{};
    if (!detail::parse_i64(line, v)) {
      return unexpected(protocol_errc::invalid_integer);
    }
    buf_.consume(consume_bytes);
    auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
    tree_.nodes.push_back(raw_node{.type = kind::integer, .text = line, .i64 = v});
    pending_attrs_.attach(tree_, idx);
    return std::optional<value_result>{value_result{.step = value_step::produced_node, .node_index = idx}};
  }

  if (*maybe_t == kind::double_number) {
    double v{};
    if (!detail::parse_double(line, v)) {
      return unexpected(protocol_errc::invalid_double);
    }
    buf_.consume(consume_bytes);
    auto idx = static_cast<std::uint32_t>(tree_.nodes.size());
    tree_.nodes.push_back(raw_node{.type = kind::double_number, .text = line, .f64 = v});
    pending_attrs_.attach(tree_, idx);
    return std::optional<value_result>{value_result{.step = value_step::produced_node, .node_index = idx}};
  }

  return unexpected(protocol_errc::invalid_type_byte);
}

inline auto parser::attach_to_parent(std::uint32_t child_idx)
  -> expected<attach_result, rediscoro::protocol_errc> {
  while (true) {
    if (stack_.empty()) {
      return attach_result{.step = attach_step::produced_root, .root_index = child_idx};
    }

    auto& parent = stack_.back();
    if (parent.state == frame_kind::array) {
      REDISCORO_ASSERT(parent.container_type == kind::array || parent.container_type == kind::set ||
                       parent.container_type == kind::push);

      auto& node = tree_.nodes.at(parent.node_index);
      if (node.child_count == 0) {
        node.first_child = static_cast<std::uint32_t>(tree_.links.size());
      }
      tree_.links.push_back(child_idx);
      parent.produced += 1;
      node.child_count = parent.produced;

      if (parent.produced == static_cast<std::uint32_t>(parent.expected)) {
        child_idx = parent.node_index;
        stack_.pop_back();
        continue;
      }

      stack_.push_back(frame{.state = frame_kind::value});
      return attach_result{.step = attach_step::continue_parsing};
    }

    if (parent.state == frame_kind::map_key) {
      REDISCORO_ASSERT(parent.container_type == kind::map);
      parent.pending_key = child_idx;
      parent.has_pending_key = true;
      parent.state = frame_kind::map_value;
      stack_.push_back(frame{.state = frame_kind::value});
      return attach_result{.step = attach_step::continue_parsing};
    }

    if (parent.state == frame_kind::map_value) {
      REDISCORO_ASSERT(parent.container_type == kind::map);
      if (!parent.has_pending_key) {
        return unexpected(protocol_errc::invalid_map_pairs);
      }

      auto& node = tree_.nodes.at(parent.node_index);
      if (node.child_count == 0) {
        node.first_child = static_cast<std::uint32_t>(tree_.links.size());
      }
      tree_.links.push_back(parent.pending_key);
      tree_.links.push_back(child_idx);
      parent.has_pending_key = false;
      parent.produced += 1;
      node.child_count = parent.produced * 2;
      parent.state = frame_kind::map_key;

      if (parent.produced == static_cast<std::uint32_t>(parent.expected)) {
        child_idx = parent.node_index;
        stack_.pop_back();
        continue;
      }

      stack_.push_back(frame{.state = frame_kind::value});
      return attach_result{.step = attach_step::continue_parsing};
    }

    if (parent.state == frame_kind::attribute) {
      REDISCORO_ASSERT(parent.container_type == kind::attribute);

      if (!parent.has_pending_key) {
        parent.pending_key = child_idx;
        parent.has_pending_key = true;
        stack_.push_back(frame{.state = frame_kind::value});
        return attach_result{.step = attach_step::continue_parsing};
      }

      pending_attrs_.push(tree_, parent.pending_key);
      pending_attrs_.push(tree_, child_idx);
      parent.has_pending_key = false;
      parent.produced += 1;

      if (parent.produced == static_cast<std::uint32_t>(parent.expected)) {
        stack_.pop_back();  // pop attribute frame
        return attach_result{.step = attach_step::continue_parsing};
      }

      stack_.push_back(frame{.state = frame_kind::value});
      return attach_result{.step = attach_step::continue_parsing};
    }

    return unexpected(protocol_errc::invalid_state);
  }
}

inline auto parser::parse_one()
  -> expected<std::optional<std::uint32_t>, rediscoro::protocol_errc> {
  if (tree_ready_) {
    return unexpected(rediscoro::protocol_errc::tree_not_consumed);
  }
  if (failed_) {
    return unexpected(rediscoro::protocol_errc::parser_failed);
  }

  if (stack_.empty()) {
    stack_.push_back(frame{.state = frame_kind::value});
  }

  while (true) {
    if (stack_.empty()) {
      failed_ = true;
      return unexpected(rediscoro::protocol_errc::invalid_state);
    }
    auto& f = stack_.back();
    switch (f.state) {
      case frame_kind::value: {
        auto r = parse_value();
        if (!r) {
          failed_ = true;
          return unexpected(r.error());
        }
        if (!r->has_value()) {
          return std::optional<std::uint32_t>{};
        }
        auto const& vr = **r;

        if (vr.step == value_step::continue_parsing) {
          break;
        }

        // Completed a node: pop value frame and attach to parent frames.
        auto child_idx = vr.node_index;
        stack_.pop_back();  // pop value frame
        auto done = attach_to_parent(child_idx);
        if (!done) {
          failed_ = true;
          return unexpected(done.error());
        }
        if (done->step == attach_step::produced_root) {
          tree_ready_ = true;
          return std::optional<std::uint32_t>{done->root_index};
        }
        break;
      }
      case frame_kind::array:
      case frame_kind::map_key:
      case frame_kind::map_value:
      case frame_kind::attribute: {
        // Containers are driven by parsing nested values.
        stack_.push_back(frame{.state = frame_kind::value});
        break;
      }
    }
  }
}

inline auto parser::reclaim() -> void {
  // After the user has consumed the raw tree, it is now safe to reclaim memory.
  tree_.reset();
  stack_.clear();
  pending_attrs_.reset();
  tree_ready_ = false;
  buf_.compact();
}

}  // namespace rediscoro::resp3
