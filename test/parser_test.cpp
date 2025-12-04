#include <gtest/gtest.h>

#include <redisus/resp3/parser.hpp>
#include <redisus/resp3/type.hpp>

// Include implementations
#include <redisus/impl/error.ipp>
#include <redisus/resp3/impl/parser.ipp>
#include <redisus/resp3/impl/type.ipp>

using namespace redisus::resp3;

// Test fixture for parser tests
class ParserTest : public ::testing::Test {
protected:
  parser p;
  std::error_code ec;

  void SetUp() override {
    ec.clear();
    p = {};
  }
};

// ============================================================================
// Simple Type Tests
// ============================================================================

TEST_F(ParserTest, SimpleString) {
  std::string_view data = "+OK\r\n";
  auto result = p.consume(data, ec);

  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::simple_string);
  EXPECT_EQ(result->value(), "OK");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, SimpleError) {
  std::string_view data = "-ERR unknown command\r\n";
  auto result = p.consume(data, ec);

  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::simple_error);
  EXPECT_EQ(result->value(), "ERR unknown command");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, Number) {
  std::string_view data = ":12345\r\n";
  auto result = p.consume(data, ec);

  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::number);
  EXPECT_EQ(result->value(), "12345");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, Double) {
  std::string_view data = ",3.14159\r\n";
  auto result = p.consume(data, ec);

  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::doublean);
  EXPECT_EQ(result->value(), "3.14159");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, BooleanTrue) {
  std::string_view data = "#t\r\n";
  auto result = p.consume(data, ec);

  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::boolean);
  EXPECT_EQ(result->value(), "t");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, Null) {
  std::string_view data = "_\r\n";
  auto result = p.consume(data, ec);

  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::null);
  EXPECT_EQ(result->value(), "");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, BigNumber) {
  std::string_view data = "(123456789012345678901234567890\r\n";
  auto result = p.consume(data, ec);

  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::big_number);
  EXPECT_EQ(result->value(), "123456789012345678901234567890");
  EXPECT_TRUE(p.done());
}

// ============================================================================
// Bulk Type Tests
// ============================================================================

TEST_F(ParserTest, BlobString) {
  std::string_view data = "$5\r\nhello\r\n";

  // Consume complete blob string
  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::blob_string);
  EXPECT_EQ(result->value(), "hello");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, EmptyBlobString) {
  std::string_view data = "$0\r\n\r\n";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::blob_string);
  EXPECT_EQ(result->value(), "");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, BlobError) {
  std::string_view data = "!21\r\nSYNTAX invalid syntax\r\n";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::blob_error);
  EXPECT_EQ(result->value(), "SYNTAX invalid syntax");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, VerbatimString) {
  std::string_view data = "=15\r\ntxt:Some string\r\n";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::verbatim_string);
  EXPECT_EQ(result->value(), "txt:Some string");
  EXPECT_TRUE(p.done());
}

// ============================================================================
// Aggregate Type Tests
// ============================================================================

TEST_F(ParserTest, EmptyArray) {
  std::string_view data = "*0\r\n";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 0u);
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, ArrayWithSimpleStrings) {
  std::string_view data = "*3\r\n+foo\r\n+bar\r\n+baz\r\n";

  // Array header
  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 3u);
  EXPECT_FALSE(p.done());

  // First element
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::simple_string);
  EXPECT_EQ(result->value(), "foo");
  EXPECT_FALSE(p.done());

  // Second element
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::simple_string);
  EXPECT_EQ(result->value(), "bar");
  EXPECT_FALSE(p.done());

  // Third element
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::simple_string);
  EXPECT_EQ(result->value(), "baz");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, Set) {
  std::string_view data = "~2\r\n+apple\r\n+banana\r\n";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::set);
  EXPECT_EQ(result->aggregate_size(), 2u);

  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->value(), "apple");

  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->value(), "banana");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, Map) {
  std::string_view data = "%2\r\n+key1\r\n+value1\r\n+key2\r\n+value2\r\n";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::map);
  EXPECT_EQ(result->aggregate_size(), 2u);

  // First key-value pair
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->value(), "key1");

  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->value(), "value1");

  // Second key-value pair
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->value(), "key2");

  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->value(), "value2");

  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, Push) {
  std::string_view data = ">2\r\n+pubsub\r\n+message\r\n";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::push);
  EXPECT_EQ(result->aggregate_size(), 2u);

  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->value(), "pubsub");

  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->value(), "message");
  EXPECT_TRUE(p.done());
}

// ============================================================================
// Nested Structure Tests
// ============================================================================

TEST_F(ParserTest, NestedArray) {
  std::string_view data = "*2\r\n*2\r\n+a\r\n+b\r\n*2\r\n+c\r\n+d\r\n";

  // Outer array
  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 2u);

  // First inner array
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 2u);

  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "a");

  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "b");

  // Second inner array
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 2u);

  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "c");

  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "d");

  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, ArrayWithMixedTypes) {
  std::string_view data = "*4\r\n+hello\r\n:42\r\n#t\r\n_\r\n";

  auto result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 4u);

  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::simple_string);
  EXPECT_EQ(result->value(), "hello");

  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::number);
  EXPECT_EQ(result->value(), "42");

  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::boolean);
  EXPECT_EQ(result->value(), "t");

  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::null);

  EXPECT_TRUE(p.done());
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(ParserTest, InvalidDataType) {
  std::string_view data = "X123\r\n";

  auto result = p.consume(data, ec);
  EXPECT_TRUE(ec);
  EXPECT_EQ(ec, redisus::error::invalid_data_type);
}

TEST_F(ParserTest, EmptyBooleanField) {
  std::string_view data = "#\r\n";

  auto result = p.consume(data, ec);
  EXPECT_TRUE(ec);
  EXPECT_EQ(ec, redisus::error::empty_field);
}

TEST_F(ParserTest, InvalidBooleanValue) {
  std::string_view data = "#x\r\n";

  auto result = p.consume(data, ec);
  EXPECT_TRUE(ec);
  EXPECT_EQ(ec, redisus::error::unexpected_bool_value);
}

TEST_F(ParserTest, EmptyNumberField) {
  std::string_view data = ":\r\n";

  auto result = p.consume(data, ec);
  EXPECT_TRUE(ec);
  EXPECT_EQ(ec, redisus::error::empty_field);
}

TEST_F(ParserTest, InvalidNumber) {
  std::string_view data = "*abc\r\n";

  auto result = p.consume(data, ec);
  EXPECT_TRUE(ec);
  EXPECT_EQ(ec, redisus::error::not_a_number);
}

// ============================================================================
// Partial Data Tests
// ============================================================================

TEST_F(ParserTest, PartialData_NoTerminator) {
  std::string_view data = "+OK";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_FALSE(result.has_value());  // Need more data
  EXPECT_FALSE(p.done());
}

TEST_F(ParserTest, PartialData_IncompleteBulk) {
  std::string_view data = "$5\r\nhel";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_FALSE(result.has_value());

  // Try to consume bulk but not enough data
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(p.done());
}

// ============================================================================
// Edge Case Tests - Strings
// ============================================================================

TEST_F(ParserTest, BlobStringWithSpecialCharacters) {
  std::string_view data = "$13\r\nHello\r\nWorld!\r\n";
  auto result = p.consume(data, ec);

  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::blob_string);
  EXPECT_EQ(result->value(), "Hello\r\nWorld!");
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, BlobStringWithNullBytes) {
  std::string data_str = "$5\r\nHe";
  data_str += '\0';
  data_str += "lo\r\n";
  std::string_view data = data_str;

  auto result = p.consume(data, ec);

  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::blob_string);
  EXPECT_EQ(result->value().length(), 5);
  EXPECT_TRUE(p.done());
}

// ============================================================================
// Edge Case Tests - Aggregates
// ============================================================================

TEST_F(ParserTest, EmptySet) {
  std::string_view data = "~0\r\n";
  auto result = p.consume(data, ec);

  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::set);
  EXPECT_EQ(result->aggregate_size(), 0u);
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, EmptyMap) {
  std::string_view data = "%0\r\n";
  auto result = p.consume(data, ec);

  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::map);
  EXPECT_EQ(result->aggregate_size(), 0u);
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, MapWithNullValues) {
  std::string_view data = "%2\r\n+key1\r\n_\r\n+key2\r\n:42\r\n";

  auto result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::map);
  EXPECT_EQ(result->aggregate_size(), 2u);

  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "key1");

  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::null);

  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "key2");

  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "42");

  EXPECT_TRUE(p.done());
}

// ============================================================================
// Edge Case Tests - Complex Nested Structures
// ============================================================================

TEST_F(ParserTest, DeeplyNestedArrays) {
  std::string_view data = "*1\r\n*1\r\n*1\r\n*1\r\n*1\r\n+deep\r\n";

  // Level 1
  auto result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 1u);

  // Level 2
  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 1u);

  // Level 3
  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 1u);

  // Level 4
  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 1u);

  // Level 5
  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 1u);

  // Value
  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "deep");

  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, MixedComplexNesting) {
  std::string_view data = "*2\r\n%1\r\n+key\r\n~2\r\n:1\r\n:2\r\n$5\r\nhello\r\n";

  // Outer array
  auto result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::array);
  EXPECT_EQ(result->aggregate_size(), 2u);

  // First element: map
  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::map);
  EXPECT_EQ(result->aggregate_size(), 1u);

  // Map key
  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "key");

  // Map value: set
  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::set);
  EXPECT_EQ(result->aggregate_size(), 2u);

  // Set elements
  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "1");

  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "2");

  // Second element: blob string
  result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::blob_string);
  EXPECT_EQ(result->value(), "hello");

  EXPECT_TRUE(p.done());
}

// ============================================================================
// Edge Case Tests - Attributes
// ============================================================================

TEST_F(ParserTest, AttributeBasic) {
  std::string_view data = "|1\r\n+key\r\n+value\r\n";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::attribute);
  EXPECT_EQ(result->aggregate_size(), 1u);

  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "key");

  result = p.consume(data, ec);
  EXPECT_EQ(result->value(), "value");

  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, AttributeEmpty) {
  std::string_view data = "|0\r\n";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->data_type, type_t::attribute);
  EXPECT_EQ(result->aggregate_size(), 0u);
  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, AttributeWithMultiplePairs) {
  std::string_view data = "|3\r\n+k1\r\n:1\r\n+k2\r\n:2\r\n+k3\r\n:3\r\n";

  auto result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::attribute);
  EXPECT_EQ(result->aggregate_size(), 3u);

  for (int i = 1; i <= 3; ++i) {
    result = p.consume(data, ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(result->value(), "k" + std::to_string(i));

    result = p.consume(data, ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(result->value(), std::to_string(i));
  }

  EXPECT_TRUE(p.done());
}

// ============================================================================
// Edge Case Tests - Streamed Strings
// ============================================================================

TEST_F(ParserTest, StreamedStringBasic) {
  std::string_view data = "$?\r\n;4\r\nHell\r\n;6\r\no worl\r\n;1\r\nd\r\n;0\r\n";

  // Streamed string header
  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::streamed_string);
  EXPECT_EQ(result->aggregate_size(), 0u);

  // Part 1
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::streamed_string_part);
  EXPECT_EQ(result->value(), "Hell");

  // Part 2
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::streamed_string_part);
  EXPECT_EQ(result->value(), "o worl");

  // Part 3
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::streamed_string_part);
  EXPECT_EQ(result->value(), "d");

  // Terminator
  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::streamed_string_part);
  EXPECT_EQ(result->value().length(), 0);

  EXPECT_TRUE(p.done());
}

TEST_F(ParserTest, StreamedStringEmpty) {
  std::string_view data = "$?\r\n;0\r\n";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::streamed_string);

  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::streamed_string_part);
  EXPECT_EQ(result->value().length(), 0);

  EXPECT_TRUE(p.done());
}

// ============================================================================
// Edge Case Tests - Partial Data with Complex Types
// ============================================================================

TEST_F(ParserTest, PartialData_ArrayHeader) {
  std::string_view data = "*3\r";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(p.done());
}

TEST_F(ParserTest, PartialData_MapWithPartialKey) {
  std::string_view data = "%1\r\n+ke";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_EQ(result->data_type, type_t::map);

  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(p.done());
}

TEST_F(ParserTest, PartialData_BlobStringLength) {
  std::string_view data = "$10";

  auto result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(p.done());
}

TEST_F(ParserTest, PartialData_StreamedStringPart) {
  std::string_view data = "$?\r\n;10\r\nabcde";

  auto result = p.consume(data, ec);
  EXPECT_EQ(result->data_type, type_t::streamed_string);

  result = p.consume(data, ec);
  ASSERT_FALSE(ec);
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(p.done());
}
