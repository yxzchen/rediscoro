#include <redisus/adapter/any_adapter.hpp>
#include <redisus/adapter/detail/impl.hpp>
#include <redisus/adapter/detail/result_traits.hpp>
#include <redisus/adapter/result.hpp>
#include <redisus/resp3/node.hpp>
#include <redisus/response.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace redisus;
using namespace redisus::adapter;

class AdapterTest : public ::testing::Test {
 protected:
  // Helper to create a simple RESP3 message with one node
  resp3::msg_view make_simple_msg(resp3::type3 type, std::string_view value) {
    node_storage.clear();
    node_storage.push_back(resp3::node_view{type, value});
    return node_storage;
  }

  resp3::msg_view make_error_msg(resp3::type3 type, std::string_view value) {
    node_storage.clear();
    node_storage.push_back(resp3::node_view{type, value});
    return node_storage;
  }

  // Helper to create an aggregate message (header + elements)
  template <typename... Args>
  resp3::msg_view make_aggregate_msg(resp3::type3 agg_type, std::size_t count, Args... element_values) {
    node_storage.clear();
    node_storage.push_back(resp3::node_view{agg_type, count});
    (node_storage.push_back(element_values), ...);
    return node_storage;
  }

  void SetUp() override { node_storage.clear(); }

  std::vector<resp3::node_view> node_storage;
};

TEST_F(AdapterTest, IntegerSimpleString) {
  response<int> res;
  any_adapter adapter(res);

  auto msg = make_simple_msg(resp3::type3::simple_string, "42");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(std::get<0>(res).has_value());
  EXPECT_EQ(std::get<0>(res).value(), 42);
}

TEST_F(AdapterTest, ThreeMessages) {
  response<std::string, int, bool> res;
  any_adapter adapter(res);
  std::error_code ec;

  // First message: string
  auto msg1 = make_simple_msg(resp3::type3::blob_string, "hello");
  adapter.on_msg(msg1, ec);
  EXPECT_FALSE(ec);

  // Second message: int
  auto msg2 = make_simple_msg(resp3::type3::number, "123");
  adapter.on_msg(msg2, ec);
  EXPECT_FALSE(ec);

  // Third message: bool
  auto msg3 = make_simple_msg(resp3::type3::boolean, "t");
  adapter.on_msg(msg3, ec);
  EXPECT_FALSE(ec);

  // Verify all results
  EXPECT_TRUE(std::get<0>(res).has_value());
  EXPECT_EQ(std::get<0>(res).value(), "hello");

  EXPECT_TRUE(std::get<1>(res).has_value());
  EXPECT_EQ(std::get<1>(res).value(), 123);

  EXPECT_TRUE(std::get<2>(res).has_value());
  EXPECT_TRUE(std::get<2>(res).value());
}

TEST_F(AdapterTest, VectorOfInts) {
  response<std::vector<int>> res;
  any_adapter adapter(res);
  std::error_code ec;

  auto msg = make_aggregate_msg(resp3::type3::array, 3,
                                  resp3::node_view{resp3::type3::number, "10"},
                                  resp3::node_view{resp3::type3::number, "20"},
                                  resp3::node_view{resp3::type3::number, "30"});
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(std::get<0>(res).has_value());
  auto const& vec = std::get<0>(res).value();
  ASSERT_EQ(vec.size(), 3);
  EXPECT_EQ(vec[0], 10);
  EXPECT_EQ(vec[1], 20);
  EXPECT_EQ(vec[2], 30);
}

TEST_F(AdapterTest, SetOfStrings) {
  response<std::set<std::string>> res;
  any_adapter adapter(res);
  std::error_code ec;

  auto msg = make_aggregate_msg(resp3::type3::set, 3,
                                  resp3::node_view{resp3::type3::blob_string, "apple"},
                                  resp3::node_view{resp3::type3::blob_string, "banana"},
                                  resp3::node_view{resp3::type3::blob_string, "cherry"});
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(std::get<0>(res).has_value());
  auto const& s = std::get<0>(res).value();
  EXPECT_EQ(s.size(), 3);
  EXPECT_TRUE(s.count("apple") == 1);
  EXPECT_TRUE(s.count("banana") == 1);
  EXPECT_TRUE(s.count("cherry") == 1);
}

TEST_F(AdapterTest, MapOfStringToInt) {
  response<std::map<std::string, int>> res;
  any_adapter adapter(res);
  std::error_code ec;

  auto msg = make_aggregate_msg(resp3::type3::map, 2,
                                  resp3::node_view{resp3::type3::blob_string, "foo"},
                                  resp3::node_view{resp3::type3::number, "100"},
                                  resp3::node_view{resp3::type3::blob_string, "bar"},
                                  resp3::node_view{resp3::type3::number, "200"});
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(std::get<0>(res).has_value());
  auto const& m = std::get<0>(res).value();
  EXPECT_EQ(m.size(), 2);
  EXPECT_EQ(m.at("foo"), 100);
  EXPECT_EQ(m.at("bar"), 200);
}

TEST_F(AdapterTest, UnorderedSetOfInts) {
  response<std::unordered_set<int>> res;
  any_adapter adapter(res);
  std::error_code ec;

  auto msg = make_aggregate_msg(resp3::type3::set, 4,
                                  resp3::node_view{resp3::type3::number, "1"},
                                  resp3::node_view{resp3::type3::number, "2"},
                                  resp3::node_view{resp3::type3::number, "3"},
                                  resp3::node_view{resp3::type3::number, "4"});
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(std::get<0>(res).has_value());
  auto const& s = std::get<0>(res).value();
  EXPECT_EQ(s.size(), 4);
  EXPECT_TRUE(s.count(1) == 1);
  EXPECT_TRUE(s.count(2) == 1);
  EXPECT_TRUE(s.count(3) == 1);
  EXPECT_TRUE(s.count(4) == 1);
}

TEST_F(AdapterTest, UnorderedMapOfIntToString) {
  response<std::unordered_map<int, std::string>> res;
  any_adapter adapter(res);
  std::error_code ec;

  auto msg = make_aggregate_msg(resp3::type3::map, 2,
                                  resp3::node_view{resp3::type3::number, "1"},
                                  resp3::node_view{resp3::type3::blob_string, "one"},
                                  resp3::node_view{resp3::type3::number, "2"},
                                  resp3::node_view{resp3::type3::blob_string, "two"});
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(std::get<0>(res).has_value());
  auto const& m = std::get<0>(res).value();
  EXPECT_EQ(m.size(), 2);
  EXPECT_EQ(m.at(1), "one");
  EXPECT_EQ(m.at(2), "two");
}

TEST_F(AdapterTest, ListOfDoubles) {
  response<std::list<double>> res;
  any_adapter adapter(res);
  std::error_code ec;

  auto msg = make_aggregate_msg(resp3::type3::array, 3,
                                  resp3::node_view{resp3::type3::doublean, "1.5"},
                                  resp3::node_view{resp3::type3::doublean, "2.5"},
                                  resp3::node_view{resp3::type3::doublean, "3.5"});
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(std::get<0>(res).has_value());
  auto const& lst = std::get<0>(res).value();
  EXPECT_EQ(lst.size(), 3);
  auto it = lst.begin();
  EXPECT_DOUBLE_EQ(*it++, 1.5);
  EXPECT_DOUBLE_EQ(*it++, 2.5);
  EXPECT_DOUBLE_EQ(*it++, 3.5);
}

TEST_F(AdapterTest, DequeOfStrings) {
  response<std::deque<std::string>> res;
  any_adapter adapter(res);
  std::error_code ec;

  auto msg = make_aggregate_msg(resp3::type3::array, 2,
                                  resp3::node_view{resp3::type3::blob_string, "first"},
                                  resp3::node_view{resp3::type3::blob_string, "second"});
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(std::get<0>(res).has_value());
  auto const& dq = std::get<0>(res).value();
  EXPECT_EQ(dq.size(), 2);
  EXPECT_EQ(dq[0], "first");
  EXPECT_EQ(dq[1], "second");
}

TEST_F(AdapterTest, ArrayOfInts) {
  response<std::array<int, 3>> res;
  any_adapter adapter(res);
  std::error_code ec;

  auto msg = make_aggregate_msg(resp3::type3::array, 3,
                                  resp3::node_view{resp3::type3::number, "5"},
                                  resp3::node_view{resp3::type3::number, "10"},
                                  resp3::node_view{resp3::type3::number, "15"});
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(std::get<0>(res).has_value());
  auto const& arr = std::get<0>(res).value();
  EXPECT_EQ(arr.size(), 3);
  EXPECT_EQ(arr[0], 5);
  EXPECT_EQ(arr[1], 10);
  EXPECT_EQ(arr[2], 15);
}
