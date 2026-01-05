#pragma once

#include <rediscoro/resp3/raw.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/assert.hpp>

#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace rediscoro::resp3 {

[[nodiscard]] inline auto build_message(const raw_tree& tree, std::uint32_t root) -> message {
  struct frame {
    std::uint32_t node = 0;
    std::uint32_t next_link = 0;  // [0..child_count) children, [child_count..child_count+attr_count) attrs
    bool initialized = false;
    bool has_parent_slot = false;
    std::uint32_t parent_slot = 0;

    message result{};

    // For map children and attrs (both are alternating key/value), hold pending key.
    std::optional<message> pending_key{};
    attribute attrs{};  // accumulate attrs here, assign to result.attrs at finalize
  };

  std::vector<frame> stack;
  stack.reserve(64);
  stack.push_back(frame{.node = root, .next_link = 0, .initialized = false, .has_parent_slot = false});

  while (!stack.empty()) {
    auto& f = stack.back();
    const auto& n = tree.nodes.at(f.node);

    if (!f.initialized) {
      f.initialized = true;

      // attribute should never be materialized
      if (n.type == type3::attribute) {
        REDISCORO_UNREACHABLE();
      }

      // Null-like: preserve type in raw tree, but current message model materializes to null.
      if (n.type == type3::null || n.i64 == -1) {
        f.result = message{null{}};
      } else {
        switch (n.type) {
          case type3::simple_string:
            f.result = message{simple_string{std::string(n.text)}};
            break;
          case type3::simple_error:
            f.result = message{simple_error{std::string(n.text)}};
            break;
          case type3::integer:
            f.result = message{integer{n.i64}};
            break;
          case type3::double_type:
            f.result = message{double_type{n.f64}};
            break;
          case type3::boolean:
            f.result = message{boolean{n.boolean}};
            break;
          case type3::big_number:
            f.result = message{big_number{std::string(n.text)}};
            break;
          case type3::bulk_string:
            f.result = message{bulk_string{std::string(n.text)}};
            break;
          case type3::bulk_error:
            f.result = message{bulk_error{std::string(n.text)}};
            break;
          case type3::verbatim_string: {
            verbatim_string v{};
            // RESP3: encoding is 3 bytes, payload is "xxx:<data>".
            // Policy: if input doesn't match this shape, fall back to encoding="txt" and keep full text as data.
            if (n.text.size() >= 4 && n.text[3] == ':') {
              v.encoding = std::string(n.text.substr(0, 3));
              v.data = std::string(n.text.substr(4));
            } else {
              v.encoding = "txt";
              v.data = std::string(n.text);
            }
            f.result = message{std::move(v)};
            break;
          }
          case type3::array: {
            array a{};
            a.elements.reserve(n.child_count);
            f.result = message{std::move(a)};
            break;
          }
          case type3::set: {
            set s{};
            s.elements.reserve(n.child_count);
            f.result = message{std::move(s)};
            break;
          }
          case type3::push: {
            push p{};
            p.elements.reserve(n.child_count);
            f.result = message{std::move(p)};
            break;
          }
          case type3::map: {
            map m{};
            m.entries.reserve(n.child_count / 2);
            f.result = message{std::move(m)};
            break;
          }
          case type3::null:
          case type3::attribute:
            REDISCORO_UNREACHABLE();
            break;
        }
      }

      if (n.attr_count > 0) {
        REDISCORO_ASSERT((n.attr_count % 2) == 0, "attr_count must be even (key/value pairs)");
        f.attrs.entries.reserve(n.attr_count / 2);
      }
      if (n.type == type3::map) {
        REDISCORO_ASSERT((n.child_count % 2) == 0, "map child_count must be even (key/value nodes)");
      }
    }

    const auto total = n.child_count + n.attr_count;
    if (f.next_link < total) {
      const auto slot = f.next_link;
      std::uint32_t child_idx{};
      if (slot < n.child_count) {
        child_idx = tree.links.at(n.first_child + slot);
      } else {
        const auto attr_i = slot - n.child_count;
        child_idx = tree.links.at(n.first_attr + attr_i);
      }
      ++f.next_link;

      stack.push_back(frame{
        .node = child_idx,
        .next_link = 0,
        .initialized = false,
        .has_parent_slot = true,
        .parent_slot = slot,
      });
      continue;
    }

    // finalize attrs
    if (n.attr_count > 0) {
      if (!f.pending_key.has_value()) {
        f.result.attrs = std::move(f.attrs);
      } else {
        // odd number of attr nodes (should not happen if parser is correct)
        REDISCORO_UNREACHABLE();
      }
    }

    // pop + attach to parent
    auto finished = std::move(f.result);
    const auto finished_slot = f.parent_slot;
    const auto has_slot = f.has_parent_slot;
    stack.pop_back();

    if (stack.empty()) {
      return finished;
    }

    auto& parent = stack.back();
    const auto& pn = tree.nodes.at(parent.node);
    REDISCORO_ASSERT(has_slot, "non-root frame must have a parent slot");

    if (finished_slot < pn.child_count) {
      // attach as child
      switch (pn.type) {
        case type3::array:
          parent.result.as<array>().elements.push_back(std::move(finished));
          break;
        case type3::set:
          parent.result.as<set>().elements.push_back(std::move(finished));
          break;
        case type3::push:
          parent.result.as<push>().elements.push_back(std::move(finished));
          break;
        case type3::map: {
          // key/value alternating
          if ((finished_slot % 2) == 0) {
            parent.pending_key = std::move(finished);
          } else {
            REDISCORO_ASSERT(parent.pending_key.has_value(), "map value without pending key");
            parent.result.as<map>().entries.push_back({std::move(*parent.pending_key), std::move(finished)});
            parent.pending_key.reset();
          }
          break;
        }
        default:
          // scalars should not have children
          REDISCORO_UNREACHABLE();
      }
    } else {
      // attach as attribute key/value
      const auto attr_i = finished_slot - pn.child_count;
      if ((attr_i % 2) == 0) {
        parent.pending_key = std::move(finished);
      } else {
        REDISCORO_ASSERT(parent.pending_key.has_value(), "attr value without pending key");
        parent.attrs.entries.push_back({std::move(*parent.pending_key), std::move(finished)});
        parent.pending_key.reset();
      }
    }
  }

  REDISCORO_UNREACHABLE();
}

}  // namespace rediscoro::resp3


