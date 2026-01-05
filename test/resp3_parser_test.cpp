#include <gtest/gtest.h>

#include <rediscoro/resp3/encoder.hpp>
#include <rediscoro/resp3/builder.hpp>
#include <rediscoro/resp3/parser.hpp>
#include <rediscoro/resp3/buffer.hpp>

#include <cstring>
#include <string_view>

using namespace rediscoro::resp3;

namespace {

auto append(buffer& b, std::string_view data) -> void {
  if (data.empty()) {
    return;
  }
  auto w = b.prepare(data.size());
  std::memcpy(w.data(), data.data(), data.size());
  b.commit(data.size());
}

TEST(resp3_parser_test, parse_simple_string_ok) {
  buffer b;
  parser p;
  append(b, "+OK\r\n");

  auto root = p.parse_one(b);
  ASSERT_TRUE(root);

  const auto& n = p.tree().nodes.at(*root);
  EXPECT_EQ(n.type, type3::simple_string);
  EXPECT_EQ(n.text, "OK");

  auto msg = build_message(p.tree(), *root);
  ASSERT_TRUE(msg.is<simple_string>());
  EXPECT_EQ(msg.as<simple_string>().data, "OK");
}

TEST(resp3_parser_test, need_more_data_does_not_modify_out) {
  buffer b;
  parser p;
  append(b, "+OK\r");

  auto root = p.parse_one(b);
  ASSERT_FALSE(root);
  EXPECT_EQ(root.error(), error::needs_more);
}

TEST(resp3_parser_test, incremental_feed_completes_message) {
  buffer b;
  parser p;
  append(b, "+O");

  auto r1 = p.parse_one(b);
  ASSERT_FALSE(r1);
  EXPECT_EQ(r1.error(), error::needs_more);

  append(b, "K\r\n");
  auto r2 = p.parse_one(b);
  ASSERT_TRUE(r2);
  auto msg = build_message(p.tree(), *r2);
  ASSERT_TRUE(msg.is<simple_string>());
  EXPECT_EQ(msg.as<simple_string>().data, "OK");
}

TEST(resp3_parser_test, parse_bulk_string_ok_and_split_payload) {
  buffer b;
  parser p;
  append(b, "$5\r\nhe");

  auto r1 = p.parse_one(b);
  ASSERT_FALSE(r1);
  EXPECT_EQ(r1.error(), error::needs_more);

  append(b, "llo\r\n");
  auto r2 = p.parse_one(b);
  ASSERT_TRUE(r2);
  const auto& n = p.tree().nodes.at(*r2);
  EXPECT_EQ(n.type, type3::bulk_string);
  EXPECT_EQ(n.text, "hello");

  auto msg = build_message(p.tree(), *r2);
  ASSERT_TRUE(msg.is<bulk_string>());
  EXPECT_EQ(msg.as<bulk_string>().data, "hello");
}

TEST(resp3_parser_test, parse_array_nested) {
  buffer b;
  parser p;
  append(b, "*2\r\n+OK\r\n:1\r\n");

  auto r = p.parse_one(b);
  ASSERT_TRUE(r);

  auto msg = build_message(p.tree(), *r);
  ASSERT_TRUE(msg.is<array>());
  const auto& elems = msg.as<array>().elements;
  ASSERT_EQ(elems.size(), 2u);
  ASSERT_TRUE(elems[0].is<simple_string>());
  EXPECT_EQ(elems[0].as<simple_string>().data, "OK");
  ASSERT_TRUE(elems[1].is<integer>());
  EXPECT_EQ(elems[1].as<integer>().value, 1);
}

TEST(resp3_parser_test, parse_message_with_attributes) {
  buffer b;
  parser p;
  append(b, "|1\r\n+key\r\n+val\r\n+OK\r\n");

  auto r = p.parse_one(b);
  ASSERT_TRUE(r);

  auto msg = build_message(p.tree(), *r);
  EXPECT_TRUE(msg.has_attributes());
  ASSERT_TRUE(msg.is<simple_string>());
  EXPECT_EQ(msg.as<simple_string>().data, "OK");

  const auto& attrs = msg.get_attributes();
  ASSERT_EQ(attrs.entries.size(), 1u);
  ASSERT_TRUE(attrs.entries[0].first.is<simple_string>());
  ASSERT_TRUE(attrs.entries[0].second.is<simple_string>());
  EXPECT_EQ(attrs.entries[0].first.as<simple_string>().data, "key");
  EXPECT_EQ(attrs.entries[0].second.as<simple_string>().data, "val");
}

TEST(resp3_parser_test, attributes_inside_aggregate_element) {
  buffer b;
  parser p;
  append(b, "*1\r\n|1\r\n+meta\r\n+1\r\n+OK\r\n");

  auto r = p.parse_one(b);
  ASSERT_TRUE(r);

  auto msg = build_message(p.tree(), *r);
  ASSERT_TRUE(msg.is<array>());
  const auto& elems = msg.as<array>().elements;
  ASSERT_EQ(elems.size(), 1u);
  EXPECT_TRUE(elems[0].has_attributes());
  ASSERT_TRUE(elems[0].is<simple_string>());
  EXPECT_EQ(elems[0].as<simple_string>().data, "OK");
}

TEST(resp3_parser_test, parse_multiple_messages_from_one_feed) {
  buffer b;
  parser p;
  append(b, "+OK\r\n:1\r\n");

  auto r1 = p.parse_one(b);
  ASSERT_TRUE(r1);
  auto m1 = build_message(p.tree(), *r1);
  ASSERT_TRUE(m1.is<simple_string>());
  EXPECT_EQ(m1.as<simple_string>().data, "OK");

  auto r2 = p.parse_one(b);
  ASSERT_TRUE(r2);
  auto m2 = build_message(p.tree(), *r2);
  ASSERT_TRUE(m2.is<integer>());
  EXPECT_EQ(m2.as<integer>().value, 1);

  auto r3 = p.parse_one(b);
  ASSERT_FALSE(r3);
  EXPECT_EQ(r3.error(), error::needs_more);
}

TEST(resp3_parser_test, protocol_error_marks_failed) {
  buffer b;
  parser p;
  append(b, "?oops\r\n");

  auto r = p.parse_one(b);
  ASSERT_FALSE(r);
  EXPECT_NE(r.error(), error::needs_more);
  EXPECT_TRUE(p.failed());

  append(b, "+OK\r\n");
  auto r2 = p.parse_one(b);
  ASSERT_FALSE(r2);
  EXPECT_TRUE(p.failed());
}

TEST(resp3_parser_test, reset_clears_failed_state) {
  buffer b;
  parser p;
  append(b, "?oops\r\n");

  auto r = p.parse_one(b);
  ASSERT_FALSE(r);
  EXPECT_TRUE(p.failed());

  p.reset();
  EXPECT_FALSE(p.failed());
  b.reset();

  append(b, "+OK\r\n");
  auto r2 = p.parse_one(b);
  ASSERT_TRUE(r2);
  auto msg = build_message(p.tree(), *r2);
  ASSERT_TRUE(msg.is<simple_string>());
  EXPECT_EQ(msg.as<simple_string>().data, "OK");
}

TEST(resp3_parser_test, protocol_error_on_bulk_string_bad_trailer) {
  buffer b;
  parser p;
  append(b, "$5\r\nhelloX\r\n");

  auto r = p.parse_one(b);
  ASSERT_FALSE(r);
  EXPECT_TRUE(p.failed());
}

TEST(resp3_parser_test, roundtrip_encoder_parser_for_complex_message) {
  message original;
  attribute attr;
  attr.entries.push_back({message(simple_string{"meta"}), message(integer{1})});

  array arr;
  arr.elements.push_back(message(simple_string{"OK"}));
  arr.elements.push_back(message(bulk_string{"hello"}));
  original = message(std::move(arr), std::move(attr));

  auto wire = encode(original);

  buffer b;
  parser p;
  append(b, wire);

  auto r = p.parse_one(b);
  ASSERT_TRUE(r);
  auto decoded = build_message(p.tree(), *r);
  EXPECT_EQ(encode(decoded), wire);
}

}  // namespace


