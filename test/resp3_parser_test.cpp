#include <gtest/gtest.h>

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

  message msg;
  auto r = p.parse_one(msg);
  EXPECT_EQ(r.status, parse_status::ok);
  EXPECT_FALSE(r.error);

  ASSERT_TRUE(msg.is<simple_string>());
  EXPECT_EQ(msg.as<simple_string>().data, "OK");
}

TEST(resp3_parser_test, need_more_data_does_not_modify_out) {
  parser p;
  append(p, "+OK\r");

  message out{simple_string{"keep"}};
  auto r = p.parse_one(out);
  EXPECT_EQ(r.status, parse_status::need_more_data);
  EXPECT_FALSE(r.error);

  ASSERT_TRUE(out.is<simple_string>());
  EXPECT_EQ(out.as<simple_string>().data, "keep");
}

TEST(resp3_parser_test, incremental_feed_completes_message) {
  parser p;
  append(p, "+O");

  message msg;
  auto r1 = p.parse_one(msg);
  EXPECT_EQ(r1.status, parse_status::need_more_data);

  append(p, "K\r\n");
  auto r2 = p.parse_one(msg);
  EXPECT_EQ(r2.status, parse_status::ok);
  ASSERT_TRUE(msg.is<simple_string>());
  EXPECT_EQ(msg.as<simple_string>().data, "OK");
}

TEST(resp3_parser_test, parse_bulk_string_ok_and_split_payload) {
  parser p;
  append(p, "$5\r\nhe");

  message msg;
  auto r1 = p.parse_one(msg);
  EXPECT_EQ(r1.status, parse_status::need_more_data);

  append(p, "llo\r\n");
  auto r2 = p.parse_one(msg);
  EXPECT_EQ(r2.status, parse_status::ok);
  ASSERT_TRUE(msg.is<bulk_string>());
  EXPECT_EQ(msg.as<bulk_string>().data, "hello");
}

TEST(resp3_parser_test, parse_array_nested) {
  parser p;
  append(p, "*2\r\n+OK\r\n:1\r\n");

  message msg;
  auto r = p.parse_one(msg);
  EXPECT_EQ(r.status, parse_status::ok);

  ASSERT_TRUE(msg.is<array>());
  const auto& elems = msg.as<array>().elements;
  ASSERT_EQ(elems.size(), 2u);
  ASSERT_TRUE(elems[0].is<simple_string>());
  EXPECT_EQ(elems[0].as<simple_string>().data, "OK");
  ASSERT_TRUE(elems[1].is<integer>());
  EXPECT_EQ(elems[1].as<integer>().value, 1);
}

TEST(resp3_parser_test, parse_message_with_attributes) {
  parser p;
  append(p, "|1\r\n+key\r\n+val\r\n+OK\r\n");

  message msg;
  auto r = p.parse_one(msg);
  EXPECT_EQ(r.status, parse_status::ok);
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
  parser p;
  append(p, "*1\r\n|1\r\n+meta\r\n+1\r\n+OK\r\n");

  message msg;
  auto r = p.parse_one(msg);
  EXPECT_EQ(r.status, parse_status::ok);

  ASSERT_TRUE(msg.is<array>());
  const auto& elems = msg.as<array>().elements;
  ASSERT_EQ(elems.size(), 1u);

  EXPECT_TRUE(elems[0].has_attributes());
  ASSERT_TRUE(elems[0].is<simple_string>());
  EXPECT_EQ(elems[0].as<simple_string>().data, "OK");
}

TEST(resp3_parser_test, parse_multiple_messages_from_one_feed) {
  parser p;
  append(p, "+OK\r\n:1\r\n");

  message m1;
  auto r1 = p.parse_one(m1);
  EXPECT_EQ(r1.status, parse_status::ok);
  ASSERT_TRUE(m1.is<simple_string>());
  EXPECT_EQ(m1.as<simple_string>().data, "OK");

  message m2;
  auto r2 = p.parse_one(m2);
  EXPECT_EQ(r2.status, parse_status::ok);
  ASSERT_TRUE(m2.is<integer>());
  EXPECT_EQ(m2.as<integer>().value, 1);

  message m3;
  auto r3 = p.parse_one(m3);
  EXPECT_EQ(r3.status, parse_status::need_more_data);
}

TEST(resp3_parser_test, protocol_error_marks_failed) {
  parser p;
  append(p, "?oops\r\n");

  message msg;
  auto r = p.parse_one(msg);
  EXPECT_EQ(r.status, parse_status::protocol_error);
  EXPECT_TRUE(r.error);
  EXPECT_TRUE(p.failed());

  append(p, "+OK\r\n");
  auto r2 = p.parse_one(msg);
  EXPECT_EQ(r2.status, parse_status::protocol_error);
  EXPECT_TRUE(p.failed());
}

TEST(resp3_parser_test, reset_clears_failed_state) {
  parser p;
  append(p, "?oops\r\n");

  message msg;
  auto r = p.parse_one(msg);
  EXPECT_EQ(r.status, parse_status::protocol_error);
  EXPECT_TRUE(p.failed());

  p.reset();
  EXPECT_FALSE(p.failed());

  append(p, "+OK\r\n");
  auto r2 = p.parse_one(msg);
  EXPECT_EQ(r2.status, parse_status::ok);
  ASSERT_TRUE(msg.is<simple_string>());
  EXPECT_EQ(msg.as<simple_string>().data, "OK");
}

TEST(resp3_parser_test, protocol_error_on_bulk_string_bad_trailer) {
  parser p;
  append(p, "$5\r\nhelloX\r\n");

  message msg;
  auto r = p.parse_one(msg);
  EXPECT_EQ(r.status, parse_status::protocol_error);
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

  message decoded;
  auto r = p.parse_one(decoded);
  EXPECT_EQ(r.status, parse_status::ok);
  EXPECT_EQ(encode(decoded), wire);
}

}  // namespace


