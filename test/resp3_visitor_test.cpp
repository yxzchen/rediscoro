#include <gtest/gtest.h>
#include <rediscoro/resp3/visitor.hpp>

#include <type_traits>

using namespace rediscoro::resp3;

namespace {

TEST(resp3_visitor_test, visit_with_lambda_callback) {
  message msg{integer{123}};

  bool visited = false;
  visit(
    [&](const auto& val) {
      using T = std::decay_t<decltype(val)>;
      if constexpr (std::is_same_v<T, integer>) {
        EXPECT_EQ(val.value, 123);
        visited = true;
      }
    },
    msg);

  EXPECT_TRUE(visited);
}

TEST(resp3_visitor_test, visit_with_return_value) {
  message msg{simple_string{"hello"}};

  auto result = visit(
    [](const auto& val) -> std::string {
      using T = std::decay_t<decltype(val)>;
      if constexpr (std::is_same_v<T, simple_string>) {
        return std::string{val.data};
      } else {
        return "unknown";
      }
    },
    msg);

  EXPECT_EQ(result, "hello");
}

TEST(resp3_visitor_test, walk_recursive_tree_traversal) {
  array inner;
  inner.elements.push_back(message{integer{1}});
  inner.elements.push_back(message{simple_string{"hello"}});

  array outer;
  outer.elements.push_back(message{simple_string{"start"}});
  outer.elements.push_back(message{std::move(inner)});

  message nested{std::move(outer)};

  int count = 0;
  walk(nested, [&](const auto&) { count++; });

  // outer array + "start" + inner array + 1 + "hello" = 5
  EXPECT_EQ(count, 5);
}

TEST(resp3_visitor_test, walk_with_attributes_included) {
  attribute attrs;
  attrs.entries.emplace_back(message{simple_string{"key"}}, message{integer{100}});

  message msg{simple_string{"value"}, std::move(attrs)};

  int count = 0;
  walk(msg, [&](const auto&) { count++; });

  // simple_string + attribute + key + value = 4
  EXPECT_EQ(count, 4);
}

}  // namespace
