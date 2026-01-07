#include <gtest/gtest.h>
#include <rediscoro/resp3/message.hpp>

using namespace rediscoro::resp3;

namespace {

TEST(type3s_test, simple_and_bulk_type_creation) {
  // Simple types
  message str{simple_string{"OK"}};
  EXPECT_EQ(str.get_type(), type3::simple_string);
  EXPECT_TRUE(str.is<simple_string>());
  EXPECT_EQ(str.as<simple_string>().data, "OK");

  message int_msg{integer{42}};
  EXPECT_EQ(int_msg.get_type(), type3::integer);
  EXPECT_EQ(int_msg.as<integer>().value, 42);

  message null_msg{null{}};
  EXPECT_TRUE(null_msg.is_null());

  // Bulk types
  message bulk{bulk_string{"hello"}};
  EXPECT_EQ(bulk.get_type(), type3::bulk_string);
  EXPECT_EQ(bulk.as<bulk_string>().data, "hello");
}

TEST(type3s_test, aggregate_types_array_and_map) {
  // Array
  array arr;
  arr.elements.push_back(message{integer{1}});
  arr.elements.push_back(message{simple_string{"two"}});

  message array_msg{std::move(arr)};
  EXPECT_EQ(array_msg.get_type(), type3::array);
  EXPECT_EQ(array_msg.as<array>().elements.size(), 2);
  EXPECT_EQ(array_msg.as<array>().elements[0].as<integer>().value, 1);

  // Map
  map m;
  m.entries.emplace_back(
    message{simple_string{"key"}},
    message{integer{100}}
  );

  message map_msg{std::move(m)};
  EXPECT_EQ(map_msg.get_type(), type3::map);
  EXPECT_EQ(map_msg.as<map>().entries.size(), 1);
  EXPECT_EQ(map_msg.as<map>().entries[0].second.as<integer>().value, 100);
}

TEST(type3s_test, attributes_attachment_and_access) {
  attribute attrs;
  attrs.entries.emplace_back(
    message{simple_string{"ttl"}},
    message{integer{3600}}
  );

  message msg{simple_string{"data"}, std::move(attrs)};

  EXPECT_TRUE(msg.has_attributes());
  EXPECT_EQ(msg.get_attributes().entries.size(), 1);
  EXPECT_EQ(msg.get_attributes().entries[0].first.as<simple_string>().data, "ttl");

  message no_attrs{integer{42}};
  EXPECT_FALSE(no_attrs.has_attributes());
}

TEST(type3s_test, nested_array_structures) {
  array inner;
  inner.elements.push_back(message{integer{1}});
  inner.elements.push_back(message{integer{2}});

  array outer;
  outer.elements.push_back(message{simple_string{"start"}});
  outer.elements.push_back(message{std::move(inner)});

  message nested{std::move(outer)};

  EXPECT_EQ(nested.as<array>().elements.size(), 2);
  EXPECT_TRUE(nested.as<array>().elements[1].is<array>());
  EXPECT_EQ(nested.as<array>().elements[1].as<array>().elements.size(), 2);
}

TEST(type3s_test, type_helper_methods) {
  message str{simple_string{"test"}};
  EXPECT_TRUE(str.is_string());
  EXPECT_TRUE(str.is_simple());
  EXPECT_FALSE(str.is_bulk());
  EXPECT_FALSE(str.is_error());

  message err{simple_error{"error"}};
  EXPECT_TRUE(err.is_error());

  message arr{array{}};
  EXPECT_TRUE(arr.is_aggregate());
}

TEST(type3s_test, static_type_id_correctness) {
  static_assert(simple_string::type_id == type3::simple_string);
  static_assert(integer::type_id == type3::integer);
  static_assert(array::type_id == type3::array);
  static_assert(null::type_id == type3::null);
}

}  // namespace
