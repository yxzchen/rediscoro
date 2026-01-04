#include <gtest/gtest.h>
#include <rediscoro/resp3/resp3.hpp>

using namespace rediscoro::resp3;

namespace {

TEST(resp3_visitor, visit_with_lambda) {
  message msg{integer{123}};

  bool visited = false;
  visit([&](const auto& val) {
    using T = std::decay_t<decltype(val)>;
    if constexpr (std::is_same_v<T, integer>) {
      EXPECT_EQ(val.value, 123);
      visited = true;
    }
  }, msg);

  EXPECT_TRUE(visited);
}

TEST(resp3_visitor, visit_return_value) {
  message msg{simple_string{"hello"}};

  auto result = visit([](const auto& val) -> std::string {
    using T = std::decay_t<decltype(val)>;
    if constexpr (std::is_same_v<T, simple_string>) {
      return val.data;
    } else {
      return "unknown";
    }
  }, msg);

  EXPECT_EQ(result, "hello");
}

TEST(resp3_visitor, walk_tree) {
  array inner;
  inner.elements.push_back(message{integer{1}});
  inner.elements.push_back(message{simple_string{"hello"}});

  array outer;
  outer.elements.push_back(message{simple_string{"start"}});
  outer.elements.push_back(message{std::move(inner)});

  message nested{std::move(outer)};

  int count = 0;
  walk(nested, [&](const auto&) {
    count++;
  });

  // outer array + "start" + inner array + 1 + "hello" = 5
  EXPECT_EQ(count, 5);
}

TEST(resp3_visitor, walk_with_attributes) {
  attribute attrs;
  attrs.entries.emplace_back(
    message{simple_string{"key"}},
    message{integer{100}}
  );

  message msg{simple_string{"value"}, std::move(attrs)};

  int count = 0;
  walk(msg, [&](const auto&) {
    count++;
  });

  // simple_string + attribute + key + value = 4
  EXPECT_EQ(count, 4);
}

TEST(resp3_visitor, generic_visitor) {
  struct counter : generic_visitor {
    int int_count = 0;
    int str_count = 0;

    auto on_integer(const integer&) -> void override {
      int_count++;
    }

    auto on_simple_string(const simple_string&) -> void override {
      str_count++;
    }
  };

  counter c;

  visit(c, message{integer{42}});
  EXPECT_EQ(c.int_count, 1);

  visit(c, message{simple_string{"hello"}});
  EXPECT_EQ(c.str_count, 1);
}

}  // namespace
