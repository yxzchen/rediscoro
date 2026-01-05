#pragma once

#include <rediscoro/resp3/raw.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/assert.hpp>

#include <string>
#include <vector>
#include <utility>

namespace rediscoro::resp3 {

[[nodiscard]] inline auto build_message(const raw_tree& tree, std::uint32_t root) -> message {
  const auto& n = tree.nodes.at(root);

  auto build_attrs = [&](message& m, const raw_node& node) -> void {
    if (node.attr_count == 0) {
      return;
    }
    attribute a{};
    a.entries.reserve(node.attr_count / 2);
    for (std::uint32_t i = 0; i < node.attr_count; i += 2) {
      auto k = tree.links.at(node.first_attr + i);
      auto v = tree.links.at(node.first_attr + i + 1);
      a.entries.push_back({build_message(tree, k), build_message(tree, v)});
    }
    m.attrs = std::move(a);
  };

  auto build_null_with_attrs = [&](const raw_node& node) -> message {
    message m{null{}};
    build_attrs(m, node);
    return m;
  };

  switch (n.type) {
    case type3::simple_string: {
      message m{simple_string{std::string(n.text)}};
      build_attrs(m, n);
      return m;
    }
    case type3::simple_error: {
      message m{simple_error{std::string(n.text)}};
      build_attrs(m, n);
      return m;
    }
    case type3::integer: {
      message m{integer{n.i64}};
      build_attrs(m, n);
      return m;
    }
    case type3::double_type: {
      message m{double_type{n.f64}};
      build_attrs(m, n);
      return m;
    }
    case type3::boolean: {
      message m{boolean{n.boolean}};
      build_attrs(m, n);
      return m;
    }
    case type3::big_number: {
      message m{big_number{std::string(n.text)}};
      build_attrs(m, n);
      return m;
    }
    case type3::null: {
      return build_null_with_attrs(n);
    }
    case type3::bulk_string: {
      if (n.i64 == -1) {
        return build_null_with_attrs(n);
      }
      message m{bulk_string{std::string(n.text)}};
      build_attrs(m, n);
      return m;
    }
    case type3::bulk_error: {
      if (n.i64 == -1) {
        return build_null_with_attrs(n);
      }
      message m{bulk_error{std::string(n.text)}};
      build_attrs(m, n);
      return m;
    }
    case type3::verbatim_string: {
      if (n.i64 == -1) {
        return build_null_with_attrs(n);
      }
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
      message m{std::move(v)};
      build_attrs(m, n);
      return m;
    }
    case type3::array: {
      if (n.i64 == -1) {
        return build_null_with_attrs(n);
      }
      array a{};
      a.elements.reserve(n.child_count);
      for (std::uint32_t i = 0; i < n.child_count; ++i) {
        auto child = tree.links.at(n.first_child + i);
        a.elements.push_back(build_message(tree, child));
      }
      message m{std::move(a)};
      build_attrs(m, n);
      return m;
    }
    case type3::set: {
      if (n.i64 == -1) {
        return build_null_with_attrs(n);
      }
      set s{};
      s.elements.reserve(n.child_count);
      for (std::uint32_t i = 0; i < n.child_count; ++i) {
        auto child = tree.links.at(n.first_child + i);
        s.elements.push_back(build_message(tree, child));
      }
      message m{std::move(s)};
      build_attrs(m, n);
      return m;
    }
    case type3::push: {
      if (n.i64 == -1) {
        return build_null_with_attrs(n);
      }
      push p{};
      p.elements.reserve(n.child_count);
      for (std::uint32_t i = 0; i < n.child_count; ++i) {
        auto child = tree.links.at(n.first_child + i);
        p.elements.push_back(build_message(tree, child));
      }
      message m{std::move(p)};
      build_attrs(m, n);
      return m;
    }
    case type3::map: {
      if (n.i64 == -1) {
        return build_null_with_attrs(n);
      }
      map mapp{};
      mapp.entries.reserve(n.child_count / 2);
      for (std::uint32_t i = 0; i < n.child_count; i += 2) {
        auto k = tree.links.at(n.first_child + i);
        auto v = tree.links.at(n.first_child + i + 1);
        mapp.entries.push_back({build_message(tree, k), build_message(tree, v)});
      }
      message m{std::move(mapp)};
      build_attrs(m, n);
      return m;
    }
    case type3::attribute: {
      // Correctness: attribute is a prefix modifier and should never be materialized as a value node.
      REDISCORO_UNREACHABLE();
    }
  }

  return message{null{}};
}

}  // namespace rediscoro::resp3


