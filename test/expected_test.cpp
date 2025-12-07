#include <redisus/expected.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace redisus;

TEST(ExpectedTest, DefaultConstruction) {
  expected<int, std::string> e;
  EXPECT_TRUE(e.has_value());
  EXPECT_TRUE(e);
  EXPECT_EQ(*e, 0);
}

TEST(ExpectedTest, ValueConstruction) {
  expected<int, std::string> e(42);
  EXPECT_TRUE(e.has_value());
  EXPECT_EQ(*e, 42);
  EXPECT_EQ(e.value(), 42);
}

TEST(ExpectedTest, UnexpectedConstruction) {
  expected<int, std::string> e(unexpected("error"));
  EXPECT_FALSE(e.has_value());
  EXPECT_FALSE(e);
  EXPECT_EQ(e.error(), "error");
}

TEST(ExpectedTest, UnexpectConstruction) {
  expected<int, std::string> e(unexpect, "error");
  EXPECT_FALSE(e.has_value());
  EXPECT_EQ(e.error(), "error");
}

TEST(ExpectedTest, InPlaceConstruction) {
  expected<std::string, int> e(std::in_place, "hello");
  EXPECT_TRUE(e.has_value());
  EXPECT_EQ(*e, "hello");
}

TEST(ExpectedTest, ValueOr) {
  expected<int, std::string> e1(42);
  expected<int, std::string> e2(unexpected("error"));

  EXPECT_EQ(e1.value_or(0), 42);
  EXPECT_EQ(e2.value_or(0), 0);
}

TEST(ExpectedTest, AndThen) {
  auto divide = [](int x, int y) -> expected<int, std::string> {
    if (y == 0) return unexpected("division by zero");
    return x / y;
  };

  expected<int, std::string> e1(10);
  auto result1 = e1.and_then([&](int x) { return divide(x, 2); });
  EXPECT_TRUE(result1.has_value());
  EXPECT_EQ(*result1, 5);

  auto result2 = e1.and_then([&](int x) { return divide(x, 0); });
  EXPECT_FALSE(result2.has_value());
  EXPECT_EQ(result2.error(), "division by zero");

  expected<int, std::string> e2(unexpected("initial error"));
  auto result3 = e2.and_then([&](int x) { return divide(x, 2); });
  EXPECT_FALSE(result3.has_value());
  EXPECT_EQ(result3.error(), "initial error");
}

TEST(ExpectedTest, Transform) {
  expected<int, std::string> e1(42);
  auto result1 = e1.transform([](int x) { return x * 2; });
  EXPECT_TRUE(result1.has_value());
  EXPECT_EQ(*result1, 84);

  expected<int, std::string> e2(unexpected("error"));
  auto result2 = e2.transform([](int x) { return x * 2; });
  EXPECT_FALSE(result2.has_value());
  EXPECT_EQ(result2.error(), "error");
}

TEST(ExpectedTest, OrElse) {
  expected<int, std::string> e1(42);
  auto result1 = e1.or_else([](std::string const& err) { return expected<int, std::string>(0); });
  EXPECT_TRUE(result1.has_value());
  EXPECT_EQ(*result1, 42);

  expected<int, std::string> e2(unexpected("error"));
  auto result2 = e2.or_else([](std::string const& err) { return expected<int, std::string>(999); });
  EXPECT_TRUE(result2.has_value());
  EXPECT_EQ(*result2, 999);
}

TEST(ExpectedTest, Equality) {
  expected<int, std::string> e1(42);
  expected<int, std::string> e2(42);
  expected<int, std::string> e3(43);
  expected<int, std::string> e4(unexpected("error"));

  EXPECT_EQ(e1, e2);
  EXPECT_NE(e1, e3);
  EXPECT_NE(e1, e4);

  EXPECT_EQ(e1, 42);
  EXPECT_EQ(42, e1);
  EXPECT_NE(e1, 43);

  EXPECT_EQ(e4, unexpected("error"));
  EXPECT_EQ(unexpected("error"), e4);
}

TEST(ExpectedTest, ArrowOperator) {
  expected<std::string, int> e("hello");
  EXPECT_EQ(e->size(), 5);
  EXPECT_EQ(e->length(), 5);
}

TEST(ExpectedTest, pointer) {
  char* c = "error";
  unexpected e(c);
  EXPECT_EQ(c, e.error());
  std::cout << (void*)c << std::endl;
  std::cout << (void*)e.error() << std::endl;
}
