#pragma once

#include <rediscoro/assert.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/resp3/raw.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rediscoro::resp3 {

namespace detail {

[[nodiscard]] inline auto is_typed_null_kind(kind t) -> bool {
  return t == kind::bulk_string || t == kind::bulk_error || t == kind::verbatim_string ||
         t == kind::array || t == kind::map || t == kind::set || t == kind::push;
}

[[nodiscard]] inline auto decode_verbatim(std::string_view payload) -> verbatim_string {
  // RESP3: encoding is 3 bytes, payload is exactly "xxx:<data>".
  REDISCORO_ASSERT(payload.size() >= 4 && payload[3] == ':', "invalid verbatim payload shape");
  return verbatim_string{
    .encoding = payload.substr(0, 3),
    .data = payload.substr(4),
  };
}

enum class parent_slot_kind : std::uint8_t {
  child,
  attribute,
};

}  // namespace detail

[[nodiscard]] inline auto build_message(const raw_tree& tree, std::uint32_t root) -> message {
  struct frame {
    std::uint32_t node = 0;

    std::uint32_t next_child = 0;
    std::uint32_t next_attr = 0;
    bool initialized = false;

    bool has_parent = false;
    detail::parent_slot_kind parent_kind{detail::parent_slot_kind::child};
    std::uint32_t parent_index = 0;  // child index within children OR attr index within attrs

    message result{};

    // Separate pending keys for map children and attrs.
    std::optional<message> pending_map_key{};
    std::optional<message> pending_attr_key{};
    attribute attrs{};  // accumulate attrs here, assign to result.attrs at finalize
  };

  std::vector<frame> stack{};
  stack.reserve(64);
  stack.push_back(frame{.node = root});

  while (!stack.empty()) {
    auto& f = stack.back();
    const auto& n = tree.nodes.at(f.node);

    if (!f.initialized) {
      f.initialized = true;

      if (n.type == kind::attribute) {
        // Attribute nodes must never be materialized (parser stores them as pending links only).
        REDISCORO_UNREACHABLE();
      }

      if (n.type == kind::null) {
        f.result = message{null{}};
      } else if (n.i64 == -1 && detail::is_typed_null_kind(n.type)) {
        // Typed nulls keep source kind: $-1/!-1/=-1/*-1/%-1/~-1/>-1
        f.result = message{null{.source = n.type}};
      } else {
        switch (n.type) {
          case kind::simple_string:
            f.result = message{simple_string{n.text}};
            break;
          case kind::simple_error:
            f.result = message{simple_error{n.text}};
            break;
          case kind::integer:
            f.result = message{integer{n.i64}};
            break;
          case kind::double_number:
            f.result = message{double_number{n.f64}};
            break;
          case kind::boolean:
            f.result = message{boolean{n.boolean}};
            break;
          case kind::big_number:
            f.result = message{big_number{n.text}};
            break;
          case kind::bulk_string:
            f.result = message{bulk_string{n.text}};
            break;
          case kind::bulk_error:
            f.result = message{bulk_error{n.text}};
            break;
          case kind::verbatim_string: {
            auto v = detail::decode_verbatim(n.text);
            f.result = message{std::move(v)};
            break;
          }
          case kind::array: {
            array a{};
            a.elements.reserve(n.child_count);
            f.result = message{std::move(a)};
            break;
          }
          case kind::set: {
            set s{};
            s.elements.reserve(n.child_count);
            f.result = message{std::move(s)};
            break;
          }
          case kind::push: {
            push p{};
            p.elements.reserve(n.child_count);
            f.result = message{std::move(p)};
            break;
          }
          case kind::map: {
            map m{};
            m.entries.reserve(n.child_count / 2);
            f.result = message{std::move(m)};
            break;
          }
          case kind::null:
          case kind::attribute:
            REDISCORO_UNREACHABLE();
            break;
        }
      }

      if (n.attr_count > 0) {
        REDISCORO_ASSERT((n.attr_count % 2) == 0, "attr_count must be even (key/value pairs)");
        f.attrs.entries.reserve(n.attr_count / 2);
      }
      if (n.type == kind::map) {
        REDISCORO_ASSERT((n.child_count % 2) == 0,
                         "map child_count must be even (key/value nodes)");
      }
    }

    // Drive child traversal first, then attributes (both preserve wire order as stored by parser).
    if (f.next_child < n.child_count) {
      const auto child_pos = f.next_child;
      const auto child_idx = tree.links.at(n.first_child + child_pos);
      f.next_child += 1;

      stack.push_back(frame{
        .node = child_idx,
        .next_child = 0,
        .next_attr = 0,
        .initialized = false,
        .has_parent = true,
        .parent_kind = detail::parent_slot_kind::child,
        .parent_index = child_pos,
      });
      continue;
    }

    if (f.next_attr < n.attr_count) {
      const auto attr_pos = f.next_attr;
      const auto attr_idx = tree.links.at(n.first_attr + attr_pos);
      f.next_attr += 1;

      stack.push_back(frame{
        .node = attr_idx,
        .next_child = 0,
        .next_attr = 0,
        .initialized = false,
        .has_parent = true,
        .parent_kind = detail::parent_slot_kind::attribute,
        .parent_index = attr_pos,
      });
      continue;
    }

    // Finalize attributes.
    if (n.attr_count > 0) {
      if (!f.pending_attr_key.has_value()) {
        f.result.attrs = std::move(f.attrs);
      } else {
        // Odd number of attr nodes (should not happen if parser is correct).
        REDISCORO_UNREACHABLE();
      }
    }

    // Pop + attach to parent.
    auto finished = std::move(f.result);
    const auto has_parent = f.has_parent;
    const auto parent_kind = f.parent_kind;
    const auto parent_index = f.parent_index;
    stack.pop_back();

    if (stack.empty()) {
      return finished;
    }

    auto& parent = stack.back();
    const auto& pn = tree.nodes.at(parent.node);
    REDISCORO_ASSERT(has_parent, "non-root frame must have a parent slot");

    if (parent_kind == detail::parent_slot_kind::child) {
      switch (pn.type) {
        case kind::array:
          parent.result.as<array>().elements.push_back(std::move(finished));
          break;
        case kind::set:
          parent.result.as<set>().elements.push_back(std::move(finished));
          break;
        case kind::push:
          parent.result.as<push>().elements.push_back(std::move(finished));
          break;
        case kind::map: {
          // Key/value alternating.
          if ((parent_index % 2) == 0) {
            parent.pending_map_key = std::move(finished);
          } else {
            REDISCORO_ASSERT(parent.pending_map_key.has_value(), "map value without pending key");
            parent.result.as<map>().entries.push_back(
              {std::move(*parent.pending_map_key), std::move(finished)});
            parent.pending_map_key.reset();
          }
          break;
        }
        default:
          // Scalars should not have children.
          REDISCORO_UNREACHABLE();
      }
    } else {
      // Attach as attribute key/value.
      if ((parent_index % 2) == 0) {
        parent.pending_attr_key = std::move(finished);
      } else {
        REDISCORO_ASSERT(parent.pending_attr_key.has_value(), "attr value without pending key");
        parent.attrs.entries.push_back({std::move(*parent.pending_attr_key), std::move(finished)});
        parent.pending_attr_key.reset();
      }
    }
  }

  REDISCORO_UNREACHABLE();
}

}  // namespace rediscoro::resp3
