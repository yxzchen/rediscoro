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
