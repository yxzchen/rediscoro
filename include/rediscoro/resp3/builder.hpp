#pragma once

#include <rediscoro/resp3/raw.hpp>
#include <rediscoro/resp3/message.hpp>

#include <string>
#include <vector>
#include <utility>

namespace rediscoro::resp3 {

[[nodiscard]] inline auto build_message(const raw_tree& tree, std::uint32_t root) -> message {
  const auto& n = tree.nodes.at(root);

  auto build_attrs = [&](message& m) -> void {
    if (n.attr_count == 0) {
      return;
    }
    attribute a{};
    a.entries.reserve(n.attr_count / 2);
    for (std::uint32_t i = 0; i < n.attr_count; i += 2) {
      auto k = tree.links.at(n.first_attr + i);
      auto v = tree.links.at(n.first_attr + i + 1);
      a.entries.push_back({build_message(tree, k), build_message(tree, v)});
    }
    m.attrs = std::move(a);
  };

  switch (n.type) {
    case raw_type::simple_string: {
      message m{simple_string{std::string(n.text)}};
      build_attrs(m);
      return m;
    }
    case raw_type::simple_error: {
      message m{simple_error{std::string(n.text)}};
      build_attrs(m);
      return m;
    }
    case raw_type::integer: {
      message m{integer{n.i64}};
      build_attrs(m);
      return m;
    }
    case raw_type::double_type: {
      message m{double_type{n.f64}};
      build_attrs(m);
      return m;
    }
    case raw_type::boolean: {
      message m{boolean{n.boolean}};
      build_attrs(m);
      return m;
    }
    case raw_type::big_number: {
      message m{big_number{std::string(n.text)}};
      build_attrs(m);
      return m;
    }
    case raw_type::null: {
      message m{null{}};
      build_attrs(m);
      return m;
    }
    case raw_type::bulk_string: {
      message m{bulk_string{std::string(n.text)}};
      build_attrs(m);
      return m;
    }
    case raw_type::bulk_error: {
      message m{bulk_error{std::string(n.text)}};
      build_attrs(m);
      return m;
    }
    case raw_type::verbatim_string: {
      verbatim_string v{};
      // Expect "xxx:<data>"
      if (n.text.size() >= 4 && n.text[3] == ':') {
        v.encoding = std::string(n.text.substr(0, 3));
        v.data = std::string(n.text.substr(4));
      } else {
        v.encoding = "txt";
        v.data = std::string(n.text);
      }
      message m{std::move(v)};
      build_attrs(m);
      return m;
    }
    case raw_type::array: {
      array a{};
      a.elements.reserve(n.child_count);
      for (std::uint32_t i = 0; i < n.child_count; ++i) {
        auto child = tree.links.at(n.first_child + i);
        a.elements.push_back(build_message(tree, child));
      }
      message m{std::move(a)};
      build_attrs(m);
      return m;
    }
    case raw_type::set: {
      set s{};
      s.elements.reserve(n.child_count);
      for (std::uint32_t i = 0; i < n.child_count; ++i) {
        auto child = tree.links.at(n.first_child + i);
        s.elements.push_back(build_message(tree, child));
      }
      message m{std::move(s)};
      build_attrs(m);
      return m;
    }
    case raw_type::push: {
      push p{};
      p.elements.reserve(n.child_count);
      for (std::uint32_t i = 0; i < n.child_count; ++i) {
        auto child = tree.links.at(n.first_child + i);
        p.elements.push_back(build_message(tree, child));
      }
      message m{std::move(p)};
      build_attrs(m);
      return m;
    }
    case raw_type::map: {
      map mapp{};
      mapp.entries.reserve(n.child_count / 2);
      for (std::uint32_t i = 0; i < n.child_count; i += 2) {
        auto k = tree.links.at(n.first_child + i);
        auto v = tree.links.at(n.first_child + i + 1);
        mapp.entries.push_back({build_message(tree, k), build_message(tree, v)});
      }
      message m{std::move(mapp)};
      build_attrs(m);
      return m;
    }
    case raw_type::attribute: {
      // Attributes are stored on `message.attrs`, never as a standalone message value.
      return message{null{}};
    }
  }

  return message{null{}};
}

}  // namespace rediscoro::resp3


