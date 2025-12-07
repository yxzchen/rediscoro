#include <redisus/resp3/parser.hpp>
#include <gtest/gtest.h>

class ParserTest : public ::testing::Test {
protected:
  void SetUp() override {}
  void TearDown() override {}
};

// === Partial Data Feeding Tests ===

TEST_F(ParserTest, PartialDataSingleByte) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Feed one byte at a time
  std::string msg = "+OK\r\n";
  for (char c : msg) {
    p.feed(std::string_view(&c, 1));
    ASSERT_TRUE(gen.next());
    auto result = gen.value();
    if (c == '\n') {
      ASSERT_TRUE(result.has_value());
      EXPECT_EQ(result->size(), 1);
    } else {
      EXPECT_FALSE(result.has_value());
    }
  }
  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, PartialDataAcrossMessageBoundary) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Feed partial first message
  p.feed("+OK");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // Complete first message and start second
  p.feed("\r\n:4");
  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);

  // Parser should be waiting for second message
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // Complete second message
  p.feed("2\r\n");
  ASSERT_TRUE(gen.next());
  result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, PartialBulkData) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Feed bulk string header
  p.feed("$5\r\n");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // Feed partial bulk data
  p.feed("hel");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // Complete bulk data without separator
  p.feed("lo");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // Feed separator
  p.feed("\r\n");
  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);
  EXPECT_EQ(result->at(0).value(), "hello");

  EXPECT_FALSE(p.error());
}

// === Multiple Nodes Tests ===

TEST_F(ParserTest, MultipleNodesSimpleArray) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Array with 3 elements
  p.feed("*3\r\n+foo\r\n+bar\r\n+baz\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 4); // array header + 3 elements

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, MultipleNodesNestedArray) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Nested array: *2\r\n*1\r\n+a\r\n*1\r\n+b\r\n
  p.feed("*2\r\n*1\r\n+a\r\n*1\r\n+b\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 5); // outer array + inner array + a + inner array + b

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, MultipleNodesDeeplyNested) {
  redisus::resp3::parser p(8192, 10);
  auto gen = p.parse();

  // Create 5 levels of nested arrays
  p.feed("*1\r\n*1\r\n*1\r\n*1\r\n*1\r\n+data\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 6); // 5 array headers + 1 data element

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, MultipleNodesMap) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Map with 3 key-value pairs (6 elements total)
  p.feed("%3\r\n+k1\r\n+v1\r\n+k2\r\n+v2\r\n+k3\r\n+v3\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 7); // map header + 6 elements

  EXPECT_FALSE(p.error());
}

// === Feeding After Need Data Tests ===

TEST_F(ParserTest, FeedAfterNeedDataMultipleTimes) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Initial feed - incomplete
  p.feed("+");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // Second feed - still incomplete
  p.feed("O");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // Third feed - still incomplete
  p.feed("K");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // Fourth feed - still incomplete
  p.feed("\r");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // Fifth feed - complete
  p.feed("\n");
  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, FeedAfterNeedDataInArray) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Array header
  p.feed("*2\r\n");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // First element incomplete
  p.feed("+f");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // First element complete
  p.feed("oo\r\n");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // Second element incomplete
  p.feed("+b");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  // Second element complete
  p.feed("ar\r\n");
  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 3);

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, FeedAfterNeedDataMultipleMessages) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Feed 3 messages with need_data states in between
  p.feed("+");
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  p.feed("msg1\r\n:");
  ASSERT_TRUE(gen.next());
  ASSERT_TRUE(gen.value().has_value());
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  p.feed("42\r\n$");
  ASSERT_TRUE(gen.next());
  ASSERT_TRUE(gen.value().has_value());
  ASSERT_TRUE(gen.next());
  EXPECT_FALSE(gen.value().has_value());

  p.feed("4\r\ntest\r\n");
  ASSERT_TRUE(gen.next());
  ASSERT_TRUE(gen.value().has_value());

  EXPECT_FALSE(p.error());
}

// === Security Tests ===

TEST_F(ParserTest, DepthLimitEnforcement) {
  redisus::resp3::parser p(8192, 3);
  auto gen = p.parse();

  // Exceed max depth of 3
  p.feed("*1\r\n*1\r\n*1\r\n*1\r\n+data\r\n");

  while (gen.next()) {
    auto result = gen.value();
    if (!result) continue;
  }

  EXPECT_EQ(p.error(), redisus::error::exceeeds_max_nested_depth);
}

TEST_F(ParserTest, AggregateOverflowProtection) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Map with size that causes overflow
  p.feed("%18446744073709551615\r\n");

  while (gen.next()) {
    auto result = gen.value();
    if (!result) continue;
  }

  EXPECT_EQ(p.error(), redisus::error::aggregate_size_overflow);
}

TEST_F(ParserTest, InvalidNumberFormat) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Number with invalid characters
  p.feed("*123abc\r\n");

  while (gen.next()) {
    auto result = gen.value();
    if (!result) continue;
  }

  EXPECT_EQ(p.error(), redisus::error::invalid_number_format);
}

TEST_F(ParserTest, StreamedStringHandling) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Streamed string with multiple parts
  p.feed("$?\r\n;3\r\nfoo\r\n;3\r\nbar\r\n;0\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result->size(), 1);

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, EmptyAggregates) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Empty array followed by empty map
  p.feed("*0\r\n%0\r\n");

  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);

  ASSERT_TRUE(gen.next());
  result = gen.value();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), 1);

  EXPECT_FALSE(p.error());
}

// === Lifetime Tests ===

TEST_F(ParserTest, StringViewLifetimeBetweenMessages) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  // Parse first message
  p.feed("$5\r\nhello\r\n");
  ASSERT_TRUE(gen.next());
  auto result1 = gen.value();
  ASSERT_TRUE(result1.has_value());

  // Store string_view from first message
  std::string_view saved_view = result1->at(0).value();
  EXPECT_EQ(saved_view, "hello");

  // Feed more data - this would trigger compaction in old code
  p.feed("$5\r\nworld\r\n");

  // The saved view should still be valid
  EXPECT_EQ(saved_view, "hello");

  // Parse second message
  ASSERT_TRUE(gen.next());
  auto result2 = gen.value();
  ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(result2->at(0).value(), "world");

  // After parsing second message, first view should still be valid
  EXPECT_EQ(saved_view, "hello");

  EXPECT_FALSE(p.error());
}

TEST_F(ParserTest, ConvertToOwningNode) {
  redisus::resp3::parser p;
  auto gen = p.parse();

  p.feed("$7\r\ntesting\r\n");
  ASSERT_TRUE(gen.next());
  auto result = gen.value();
  ASSERT_TRUE(result.has_value());

  // Convert to owning node
  auto owning = redisus::resp3::to_owning_nodes(*result);
  EXPECT_EQ(owning.size(), 1);
  EXPECT_EQ(owning[0].value(), "testing");

  // Feed lots of data to trigger buffer operations
  for (int i = 0; i < 100; ++i) {
    p.feed("$4\r\ntest\r\n");
    ASSERT_TRUE(gen.next());
    gen.value();
  }

  // Owning node should still be valid
  EXPECT_EQ(owning[0].value(), "testing");

  EXPECT_FALSE(p.error());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
