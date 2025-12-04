/* Copyright (c) 2018-2024 Marcelo Zimbres Silva (mzimbres@gmail.com)
 *
 * Distributed under the Boost Software License, Version 1.0. (See
 * accompanying file LICENSE.txt)
 */

#include <gtest/gtest.h>

#include <redisus/adapter/adapt.hpp>
#include <redisus/adapter/adapter.hpp>
#include <redisus/adapter/adapters.hpp>
#include <redisus/resp3/parser.hpp>

// Include implementations
#include <redisus/impl/error.ipp>
#include <redisus/resp3/impl/parser.ipp>
#include <redisus/resp3/impl/type.ipp>

#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

using namespace redisus;

TEST(AdapterTest, SimpleString) {
  std::string result;
  std::error_code ec;

  auto success = adapter::parse("+OK\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_EQ(result, "OK");
}

TEST(AdapterTest, BulkString) {
  std::string result;
  std::error_code ec;

  auto success = adapter::parse("$5\r\nhello\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_EQ(result, "hello");
}

TEST(AdapterTest, Number) {
  int result = 0;
  std::error_code ec;

  auto success = adapter::parse(":42\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_EQ(result, 42);
}

TEST(AdapterTest, NegativeNumber) {
  int result = 0;
  std::error_code ec;

  auto success = adapter::parse(":-100\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_EQ(result, -100);
}

TEST(AdapterTest, Double) {
  double result = 0.0;
  std::error_code ec;

  auto success = adapter::parse(",3.14\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_DOUBLE_EQ(result, 3.14);
}

TEST(AdapterTest, BooleanTrue) {
  bool result = false;
  std::error_code ec;

  auto success = adapter::parse("#t\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_TRUE(result);
}

TEST(AdapterTest, BooleanFalse) {
  bool result = true;
  std::error_code ec;

  auto success = adapter::parse("#f\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_FALSE(result);
}

TEST(AdapterTest, Null_String) {
  std::string result = "not empty";
  std::error_code ec;

  auto success = adapter::parse("_\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_TRUE(result.empty());
}

TEST(AdapterTest, Optional_WithValue) {
  std::optional<std::string> result;
  std::error_code ec;

  auto success = adapter::parse("+hello\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "hello");
}

TEST(AdapterTest, Optional_Null) {
  std::optional<std::string> result = "should be cleared";
  std::error_code ec;

  auto success = adapter::parse("_\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_FALSE(result.has_value());
}

TEST(AdapterTest, Optional_Number) {
  std::optional<int> result;
  std::error_code ec;

  auto success = adapter::parse(":123\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 123);
}

TEST(AdapterTest, Vector_SimpleStrings) {
  std::vector<std::string> result;
  std::error_code ec;

  auto success = adapter::parse("*3\r\n+one\r\n+two\r\n+three\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0], "one");
  EXPECT_EQ(result[1], "two");
  EXPECT_EQ(result[2], "three");
}

TEST(AdapterTest, Vector_BulkStrings) {
  std::vector<std::string> result;
  std::error_code ec;

  auto success = adapter::parse("*2\r\n$5\r\nhello\r\n$5\r\nworld\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result[0], "hello");
  EXPECT_EQ(result[1], "world");
}

TEST(AdapterTest, Vector_Numbers) {
  std::vector<int> result;
  std::error_code ec;

  auto success = adapter::parse("*4\r\n:1\r\n:2\r\n:3\r\n:4\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  ASSERT_EQ(result.size(), 4u);
  EXPECT_EQ(result[0], 1);
  EXPECT_EQ(result[1], 2);
  EXPECT_EQ(result[2], 3);
  EXPECT_EQ(result[3], 4);
}

TEST(AdapterTest, Vector_Empty) {
  std::vector<std::string> result;
  std::error_code ec;

  auto success = adapter::parse("*0\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_TRUE(result.empty());
}

TEST(AdapterTest, Map_SimpleStrings) {
  std::map<std::string, std::string> result;
  std::error_code ec;

  auto success = adapter::parse("%2\r\n+key1\r\n+value1\r\n+key2\r\n+value2\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result["key1"], "value1");
  EXPECT_EQ(result["key2"], "value2");
}

TEST(AdapterTest, Map_MixedTypes) {
  std::map<std::string, int> result;
  std::error_code ec;

  auto success = adapter::parse("%2\r\n+age\r\n:30\r\n+score\r\n:100\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  ASSERT_EQ(result.size(), 2u);
  EXPECT_EQ(result["age"], 30);
  EXPECT_EQ(result["score"], 100);
}

TEST(AdapterTest, Map_Empty) {
  std::map<std::string, std::string> result;
  std::error_code ec;

  auto success = adapter::parse("%0\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_TRUE(result.empty());
}

TEST(AdapterTest, Error_AggregateToSimpleType) {
  std::string result;
  std::error_code ec;

  auto success = adapter::parse("*2\r\n+one\r\n+two\r\n", result, ec);

  EXPECT_TRUE(ec);
  EXPECT_EQ(ec, error::expects_resp3_simple_type);
}

TEST(AdapterTest, Error_InvalidNumber) {
  int result = 0;
  std::error_code ec;

  auto success = adapter::parse("+notanumber\r\n", result, ec);

  EXPECT_TRUE(ec);
  EXPECT_EQ(ec, error::not_a_number);
}

TEST(AdapterTest, Vector_WithMixedBulkAndSimple) {
  std::vector<std::string> result;
  std::error_code ec;

  auto success = adapter::parse("*3\r\n+simple\r\n$4\r\nbulk\r\n+another\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0], "simple");
  EXPECT_EQ(result[1], "bulk");
  EXPECT_EQ(result[2], "another");
}

TEST(AdapterTest, Set_Strings) {
  std::vector<std::string> result;
  std::error_code ec;

  // Sets in RESP3 are like arrays
  auto success = adapter::parse("~3\r\n+a\r\n+b\r\n+c\r\n", result, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0], "a");
  EXPECT_EQ(result[1], "b");
  EXPECT_EQ(result[2], "c");
}

TEST(AdapterTest, ManualAdapter_SimpleString) {
  std::string result;
  std::error_code ec;

  resp3::parser p;
  auto adapter = adapter::make_adapter(result);

  auto success = resp3::parse(p, "+hello\r\n", adapter, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  EXPECT_EQ(result, "hello");
}

TEST(AdapterTest, ManualAdapter_Vector) {
  std::vector<int> result;
  std::error_code ec;

  resp3::parser p;
  auto adapter = adapter::make_adapter(result);

  auto success = resp3::parse(p, "*3\r\n:10\r\n:20\r\n:30\r\n", adapter, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
  ASSERT_EQ(result.size(), 3u);
  EXPECT_EQ(result[0], 10);
  EXPECT_EQ(result[1], 20);
  EXPECT_EQ(result[2], 30);
}

TEST(AdapterTest, Ignore_SimpleString) {
  std::error_code ec;

  resp3::parser p;
  auto success = resp3::parse(p, "+OK\r\n", adapter::ignore, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
}

TEST(AdapterTest, Ignore_Array) {
  std::error_code ec;

  resp3::parser p;
  auto success = resp3::parse(p, "*3\r\n+one\r\n+two\r\n+three\r\n", adapter::ignore, ec);

  EXPECT_TRUE(success);
  EXPECT_FALSE(ec);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
