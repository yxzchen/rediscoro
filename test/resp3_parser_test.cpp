#include <gtest/gtest.h>

#include <rediscoro/resp3/builder.hpp>
#include <rediscoro/resp3/encoder.hpp>
#include <rediscoro/resp3/parser.hpp>

#include <cstring>
#include <string_view>

using namespace rediscoro::resp3;

namespace {

auto append(parser& p, std::string_view data) -> void {
  if (data.empty()) {
    return;
  }
  auto w = p.prepare(data.size());
  std::memcpy(w.data(), data.data(), data.size());
  p.commit(data.size());
}

TEST(resp3_parser_test, parse_simple_string_ok) {
  parser p;
  append(p, "+OK\r\n");

  auto root = p.parse_one();
  ASSERT_TRUE(root);
  ASSERT_FALSE(root->need_more());

  const auto idx = root->value;
  const auto& n = p.tree().nodes.at(idx);
  EXPECT_EQ(n.type, kind::simple_string);
  EXPECT_EQ(n.text, "OK");

  auto msg = build_message(p.tree(), idx);
  ASSERT_TRUE(msg.is<simple_string>());
  EXPECT_EQ(msg.as<simple_string>().data, "OK");
  p.reclaim();
}

TEST(resp3_parser_test, need_more_data_does_not_modify_out) {
  parser p;
  append(p, "+OK\r");

  auto root = p.parse_one();
  ASSERT_TRUE(root);
  EXPECT_TRUE(root->need_more());
}

TEST(resp3_parser_test, incremental_feed_completes_message) {
  parser p;
  append(p, "+O");

  auto r1 = p.parse_one();
  ASSERT_TRUE(r1);
  EXPECT_TRUE(r1->need_more());

  append(p, "K\r\n");
  auto r2 = p.parse_one();
  ASSERT_TRUE(r2);
  ASSERT_FALSE(r2->need_more());
  auto msg = build_message(p.tree(), r2->value);
  ASSERT_TRUE(msg.is<simple_string>());
  EXPECT_EQ(msg.as<simple_string>().data, "OK");
  p.reclaim();
}

TEST(resp3_parser_test, parse_bulk_string_ok_and_split_payload) {
  parser p;
  append(p, "$5\r\nhe");

  auto r1 = p.parse_one();
  ASSERT_TRUE(r1);
  EXPECT_TRUE(r1->need_more());

  append(p, "llo\r\n");
  auto r2 = p.parse_one();
  ASSERT_TRUE(r2);
  ASSERT_FALSE(r2->need_more());
  const auto idx = r2->value;
  const auto& n = p.tree().nodes.at(idx);
  EXPECT_EQ(n.type, kind::bulk_string);
  EXPECT_EQ(n.text, "hello");

  auto msg = build_message(p.tree(), idx);
  ASSERT_TRUE(msg.is<bulk_string>());
  EXPECT_EQ(msg.as<bulk_string>().data, "hello");
  p.reclaim();
}

TEST(resp3_parser_test, parse_array_nested) {
  parser p;
  append(p, "*2\r\n+OK\r\n:1\r\n");

  auto r = p.parse_one();
  ASSERT_TRUE(r);
  ASSERT_FALSE(r->need_more());

  auto msg = build_message(p.tree(), r->value);
  ASSERT_TRUE(msg.is<array>());
  const auto& elems = msg.as<array>().elements;
  ASSERT_EQ(elems.size(), 2u);
  ASSERT_TRUE(elems[0].is<simple_string>());
  EXPECT_EQ(elems[0].as<simple_string>().data, "OK");
  ASSERT_TRUE(elems[1].is<integer>());
  EXPECT_EQ(elems[1].as<integer>().value, 1);
  p.reclaim();
}

TEST(resp3_parser_test, parse_message_with_attributes) {
  parser p;
  append(p, "|1\r\n+key\r\n+val\r\n+OK\r\n");

  auto r = p.parse_one();
  ASSERT_TRUE(r);
  ASSERT_FALSE(r->need_more());

  auto msg = build_message(p.tree(), r->value);
  EXPECT_TRUE(msg.has_attributes());
  ASSERT_TRUE(msg.is<simple_string>());
  EXPECT_EQ(msg.as<simple_string>().data, "OK");

  const auto& attrs = msg.get_attributes();
  ASSERT_EQ(attrs.entries.size(), 1u);
  ASSERT_TRUE(attrs.entries[0].first.is<simple_string>());
  ASSERT_TRUE(attrs.entries[0].second.is<simple_string>());
  EXPECT_EQ(attrs.entries[0].first.as<simple_string>().data, "key");
  EXPECT_EQ(attrs.entries[0].second.as<simple_string>().data, "val");
  p.reclaim();
}

TEST(resp3_parser_test, attributes_inside_aggregate_element) {
  parser p;
  append(p, "*1\r\n|1\r\n+meta\r\n+1\r\n+OK\r\n");

  auto r = p.parse_one();
  ASSERT_TRUE(r);
  ASSERT_FALSE(r->need_more());

  auto msg = build_message(p.tree(), r->value);
  ASSERT_TRUE(msg.is<array>());
  const auto& elems = msg.as<array>().elements;
  ASSERT_EQ(elems.size(), 1u);
  EXPECT_TRUE(elems[0].has_attributes());
  ASSERT_TRUE(elems[0].is<simple_string>());
  EXPECT_EQ(elems[0].as<simple_string>().data, "OK");
  p.reclaim();
}

TEST(resp3_parser_test, parse_multiple_messages_from_one_feed) {
  parser p;
  append(p, "+OK\r\n:1\r\n");

  auto r1 = p.parse_one();
  ASSERT_TRUE(r1);
  ASSERT_FALSE(r1->need_more());
  auto m1 = build_message(p.tree(), r1->value);
  ASSERT_TRUE(m1.is<simple_string>());
  EXPECT_EQ(m1.as<simple_string>().data, "OK");
  p.reclaim();

  auto r2 = p.parse_one();
  ASSERT_TRUE(r2);
  ASSERT_FALSE(r2->need_more());
  auto m2 = build_message(p.tree(), r2->value);
  ASSERT_TRUE(m2.is<integer>());
  EXPECT_EQ(m2.as<integer>().value, 1);
  p.reclaim();

  auto r3 = p.parse_one();
  ASSERT_TRUE(r3);
  EXPECT_TRUE(r3->need_more());
}

TEST(resp3_parser_test, protocol_error_marks_failed) {
  parser p;
  append(p, "?oops\r\n");

  auto r = p.parse_one();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), rediscoro::protocol_errc::invalid_type_byte);
  EXPECT_TRUE(p.failed());

  append(p, "+OK\r\n");
  auto r2 = p.parse_one();
  ASSERT_FALSE(r2);
  EXPECT_TRUE(p.failed());
}

TEST(resp3_parser_test, reset_clears_failed_state) {
  parser p;
  append(p, "?oops\r\n");

  auto r = p.parse_one();
  ASSERT_FALSE(r);
  EXPECT_TRUE(p.failed());

  p.reset();
  EXPECT_FALSE(p.failed());

  append(p, "+OK\r\n");
  auto r2 = p.parse_one();
  ASSERT_TRUE(r2);
  ASSERT_FALSE(r2->need_more());
  auto msg = build_message(p.tree(), r2->value);
  ASSERT_TRUE(msg.is<simple_string>());
  EXPECT_EQ(msg.as<simple_string>().data, "OK");
  p.reclaim();
}

TEST(resp3_parser_test, protocol_error_on_bulk_string_bad_trailer) {
  parser p;
  append(p, "$5\r\nhelloX\r\n");

  auto r = p.parse_one();
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), rediscoro::protocol_errc::invalid_bulk_trailer);
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

  parser p;
  append(p, wire);

  auto r = p.parse_one();
  ASSERT_TRUE(r);
  ASSERT_FALSE(r->need_more());
  auto decoded = build_message(p.tree(), r->value);
  EXPECT_EQ(encode(decoded), wire);
  p.reclaim();
}

}  // namespace
