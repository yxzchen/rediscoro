#include <gtest/gtest.h>
#include <rediscoro/resp3/encoder.hpp>

#include <cmath>
#include <limits>

using namespace rediscoro::resp3;

namespace {

TEST(resp3_encoder_test, encode_all_simple_types) {
  // Simple string
  message str{simple_string{"OK"}};
  EXPECT_EQ(encode(str), "+OK\r\n");

  // Simple error
  message err{simple_error{"ERR something wrong"}};
  EXPECT_EQ(encode(err), "-ERR something wrong\r\n");

  // Integer
  message int_msg{integer{42}};
  EXPECT_EQ(encode(int_msg), ":42\r\n");

  message neg_int{integer{-123}};
  EXPECT_EQ(encode(neg_int), ":-123\r\n");

  // Double
  message dbl{double_number{3.14}};
  auto encoded = encode(dbl);
  EXPECT_EQ(encoded.substr(0, 1), ",");
  EXPECT_TRUE(encoded.find("3.14") != std::string::npos);

  // Boolean
  message bool_true{boolean{true}};
  EXPECT_EQ(encode(bool_true), "#t\r\n");

  message bool_false{boolean{false}};
  EXPECT_EQ(encode(bool_false), "#f\r\n");

  // Big number
  message big{big_number{"12345678901234567890"}};
  EXPECT_EQ(encode(big), "(12345678901234567890\r\n");

  // Null
  message null_msg{null{}};
  EXPECT_EQ(encode(null_msg), "_\r\n");
}

TEST(resp3_encoder_test, encode_double_special_values) {
  message pos_inf{double_number{std::numeric_limits<double>::infinity()}};
  EXPECT_EQ(encode(pos_inf), ",inf\r\n");

  message neg_inf{double_number{-std::numeric_limits<double>::infinity()}};
  EXPECT_EQ(encode(neg_inf), ",-inf\r\n");

  message nan_msg{double_number{std::numeric_limits<double>::quiet_NaN()}};
  EXPECT_EQ(encode(nan_msg), ",nan\r\n");
}

TEST(resp3_encoder_test, encode_bulk_types_with_length) {
  // Bulk string
  message bulk{bulk_string{"hello"}};
  EXPECT_EQ(encode(bulk), "$5\r\nhello\r\n");

  message empty_bulk{bulk_string{""}};
  EXPECT_EQ(encode(empty_bulk), "$0\r\n\r\n");

  // Bulk error
  message bulk_err{bulk_error{"error message"}};
  EXPECT_EQ(encode(bulk_err), "!13\r\nerror message\r\n");

  // Verbatim string
  verbatim_string vstr;
  vstr.encoding = "txt";
  vstr.data = "hello";
  message vstr_msg{std::move(vstr)};
  EXPECT_EQ(encode(vstr_msg), "=9\r\ntxt:hello\r\n");
}

TEST(resp3_encoder_test, encode_array_aggregate) {
  // Simple array
  array arr;
  arr.elements.push_back(message{integer{1}});
  arr.elements.push_back(message{integer{2}});
  arr.elements.push_back(message{integer{3}});

  message arr_msg{std::move(arr)};
  EXPECT_EQ(encode(arr_msg), "*3\r\n:1\r\n:2\r\n:3\r\n");

  // Empty array
  message empty_arr{array{}};
  EXPECT_EQ(encode(empty_arr), "*0\r\n");

  // Mixed types
  array mixed;
  mixed.elements.push_back(message{simple_string{"hello"}});
  mixed.elements.push_back(message{integer{42}});
  mixed.elements.push_back(message{null{}});

  message mixed_msg{std::move(mixed)};
  EXPECT_EQ(encode(mixed_msg), "*3\r\n+hello\r\n:42\r\n_\r\n");
}

TEST(resp3_encoder_test, encode_map_with_ordered_entries) {
  map m;
  m.entries.emplace_back(message{simple_string{"key1"}}, message{simple_string{"value1"}});
  m.entries.emplace_back(message{simple_string{"key2"}}, message{integer{42}});

  message map_msg{std::move(m)};
  EXPECT_EQ(encode(map_msg), "%2\r\n+key1\r\n+value1\r\n+key2\r\n:42\r\n");

  // Empty map
  message empty_map{map{}};
  EXPECT_EQ(encode(empty_map), "%0\r\n");
}

TEST(resp3_encoder_test, encode_set_aggregate) {
  set s;
  s.elements.push_back(message{simple_string{"a"}});
  s.elements.push_back(message{simple_string{"b"}});
  s.elements.push_back(message{simple_string{"c"}});

  message set_msg{std::move(s)};
  EXPECT_EQ(encode(set_msg), "~3\r\n+a\r\n+b\r\n+c\r\n");
}

TEST(resp3_encoder_test, encode_push_message) {
  push p;
  p.elements.push_back(message{simple_string{"pubsub"}});
  p.elements.push_back(message{simple_string{"message"}});
  p.elements.push_back(message{simple_string{"hello"}});

  message push_msg{std::move(p)};
  EXPECT_EQ(encode(push_msg), ">3\r\n+pubsub\r\n+message\r\n+hello\r\n");
}

TEST(resp3_encoder_test, encode_nested_arrays) {
  // Nested array
  array inner;
  inner.elements.push_back(message{integer{1}});
  inner.elements.push_back(message{integer{2}});

  array outer;
  outer.elements.push_back(message{simple_string{"start"}});
  outer.elements.push_back(message{std::move(inner)});
  outer.elements.push_back(message{simple_string{"end"}});

  message nested{std::move(outer)};
  EXPECT_EQ(encode(nested), "*3\r\n+start\r\n*2\r\n:1\r\n:2\r\n+end\r\n");
}

TEST(resp3_encoder_test, encode_message_with_attributes) {
  attribute attrs;
  attrs.entries.emplace_back(message{simple_string{"ttl"}}, message{integer{3600}});

  message msg{simple_string{"cached_value"}, std::move(attrs)};
  EXPECT_EQ(encode(msg), "|1\r\n+ttl\r\n:3600\r\n+cached_value\r\n");
}

TEST(resp3_encoder_test, encode_complex_redis_response) {
  // HGETALL response with attributes
  map m;
  m.entries.emplace_back(message{simple_string{"name"}}, message{bulk_string{"Alice"}});
  m.entries.emplace_back(message{simple_string{"age"}}, message{bulk_string{"30"}});

  attribute attrs;
  attrs.entries.emplace_back(message{simple_string{"db"}}, message{integer{0}});

  message response{std::move(m), std::move(attrs)};

  auto encoded = encode(response);
  // Should be: attributes first, then map
  EXPECT_TRUE(encoded.starts_with("|1\r\n"));
  EXPECT_TRUE(encoded.find("%2\r\n") != std::string::npos);
}

TEST(resp3_encoder_test, encoder_reuse_clears_buffer) {
  encoder enc;

  message msg1{simple_string{"test1"}};
  auto result1 = enc.encode(msg1);
  EXPECT_EQ(result1, "+test1\r\n");

  message msg2{integer{42}};
  auto result2 = enc.encode(msg2);
  EXPECT_EQ(result2, ":42\r\n");

  // Verify first result wasn't modified
  EXPECT_EQ(result1, "+test1\r\n");
}

TEST(resp3_encoder_test, encode_to_appends_to_buffer) {
  encoder enc;
  std::string buffer;

  message msg1{simple_string{"hello"}};
  enc.encode_to(buffer, msg1);

  message msg2{integer{42}};
  enc.encode_to(buffer, msg2);

  EXPECT_EQ(buffer, "+hello\r\n:42\r\n");
}

}  // namespace
