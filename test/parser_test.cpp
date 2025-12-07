#include <redisus/resp3/parser.hpp>
#include <redisus/resp3/impl/parser.ipp>
#include <redisus/resp3/impl/type.ipp>
#include <redisus/impl/error.ipp>
#include <gtest/gtest.h>

class ParserTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

// === Partial Feeding Tests ===

TEST_F(ParserTest, PartialSimpleString) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  p.feed("+OK");
  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  EXPECT_FALSE(result.has_value());

  p.feed("\r\n");
  ASSERT_TRUE(gen.next());
  result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);
  EXPECT_EQ(result->at(0).data_type, redisus::resp3::type3::simple_string);
  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, PartialArray) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  p.feed("*2\r\n+f");
  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  EXPECT_FALSE(result.has_value());

  p.feed("oo\r\n+ba");
  ASSERT_TRUE(gen.next());
  result = gen.value();
  EXPECT_FALSE(result.has_value());

  p.feed("r\r\n");
  ASSERT_TRUE(gen.next());
  result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 3);
  EXPECT_EQ(result->at(0).data_type, redisus::resp3::type3::array);
  EXPECT_EQ(result->at(1).data_type, redisus::resp3::type3::simple_string);
  EXPECT_EQ(result->at(2).data_type, redisus::resp3::type3::simple_string);
  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, PartialNumber) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  p.feed(":");
  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  EXPECT_FALSE(result.has_value());

  p.feed("42\r\n");
  ASSERT_TRUE(gen.next());
  result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);
  EXPECT_EQ(result->at(0).data_type, redisus::resp3::type3::number);
  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, MultipleMessages) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  p.feed("+OK\r\n:42\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);
  EXPECT_EQ(result->at(0).data_type, redisus::resp3::type3::simple_string);

  ASSERT_TRUE(gen.next());
  result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);
  EXPECT_EQ(result->at(0).data_type, redisus::resp3::type3::number);

  EXPECT_FALSE(p.error());
}

// === Security Tests ===

TEST_F(ParserTest, DepthLimitProtection) {
  redisus::resp3::parser p(8192, 3);
  auto gen = p.parse();

  // Create 4 levels of arrays (exceeds max depth of 3)
  p.feed("*1\r\n*1\r\n*1\r\n*1\r\n:1\r\n");

  while (gen.next()) {
    auto result = gen.value();
    if (!result) continue;
  }

  EXPECT_EQ(p.error(), redisus::error::exceeeds_max_nested_depth);
}

TEST_F(ParserTest, IntegerOverflowProtection) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Map with size that would overflow (SIZE_MAX causes overflow with multiplicity 2)
  p.feed("%18446744073709551615\r\n");

  while (gen.next()) {
    auto result = gen.value();
    if (!result) continue;
  }

  EXPECT_EQ(p.error(), redisus::error::aggregate_size_overflow);
}

TEST_F(ParserTest, NumberValidation) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Invalid number format: "123abc"
  p.feed("*123abc\r\n");

  while (gen.next()) {
    auto result = gen.value();
    if (!result) continue;
  }

  EXPECT_EQ(p.error(), redisus::error::invalid_number_format);
}

TEST_F(ParserTest, ValidNestedStructure) {
  redisus::resp3::parser p(8192, 5);
  auto gen = p.parse();

  // Nested array within depth limit
  p.feed("*1\r\n*1\r\n:42\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 3);

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, StreamedString) {
  redisus::resp3::parser p(8192, 2);
  auto gen = p.parse();

  // Streamed string: $?\r\n;3\r\nfoo\r\n;0\r\n
  p.feed("$?\r\n;3\r\nfoo\r\n;0\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->at(0).data_type, redisus::resp3::type3::streamed_string);

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, EmptyArray) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  p.feed("*0\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);
  EXPECT_EQ(result->at(0).data_type, redisus::resp3::type3::array);
  EXPECT_EQ(result->at(0).aggregate_size(), 0);

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, BulkString) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  p.feed("$5\r\nhello\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);
  EXPECT_EQ(result->at(0).data_type, redisus::resp3::type3::blob_string);
  EXPECT_EQ(result->at(0).value(), "hello");

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, BooleanValues) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  p.feed("#t\r\n#f\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->at(0).data_type, redisus::resp3::type3::boolean);
  EXPECT_EQ(result->at(0).value(), "t");

  ASSERT_TRUE(gen.next());
  result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->at(0).data_type, redisus::resp3::type3::boolean);
  EXPECT_EQ(result->at(0).value(), "f");

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, InvalidBooleanValue) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  p.feed("#x\r\n");

  while (gen.next()) {
    auto result = gen.value();
    if (!result) continue;
  }

  EXPECT_EQ(p.error(), redisus::error::unexpected_bool_value);
}

TEST_F(ParserTest, Map) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  p.feed("%2\r\n+key1\r\n+val1\r\n+key2\r\n+val2\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 5); // map header + 4 elements
  EXPECT_EQ(result->at(0).data_type, redisus::resp3::type3::map);
  EXPECT_EQ(result->at(0).aggregate_size(), 2);

  EXPECT_FALSE(p.error());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
