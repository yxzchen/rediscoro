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

TEST_F(AdapterTest, IntegerBlobString) {
  result<int> res;
  auto adapter = detail::result_traits<result<int>>::adapt(res);

  auto msg = make_simple_msg(resp3::type3::blob_string, "123");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(res.value(), 123);
}

TEST_F(AdapterTest, IntegerNumber) {
  result<int> res;
  auto adapter = detail::result_traits<result<int>>::adapt(res);

  auto msg = make_simple_msg(resp3::type3::number, "999");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(res.value(), 999);
}

TEST_F(AdapterTest, StringSimple) {
  result<std::string> res;
  auto adapter = detail::result_traits<result<std::string>>::adapt(res);

  auto msg = make_simple_msg(resp3::type3::simple_string, "hello");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(res.value(), "hello");
}

TEST_F(AdapterTest, StringBlob) {
  result<std::string> res;
  auto adapter = detail::result_traits<result<std::string>>::adapt(res);

  auto msg = make_simple_msg(resp3::type3::blob_string, "world");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(res.has_value());
  EXPECT_EQ(res.value(), "world");
}

TEST_F(AdapterTest, Boolean) {
  result<bool> res;
  auto adapter = detail::result_traits<result<bool>>::adapt(res);

  auto msg = make_simple_msg(resp3::type3::boolean, "t");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(res.has_value());
  EXPECT_TRUE(res.value());
}

TEST_F(AdapterTest, BooleanFalse) {
  result<bool> res;
  auto adapter = detail::result_traits<result<bool>>::adapt(res);

  auto msg = make_simple_msg(resp3::type3::boolean, "f");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(res.has_value());
  EXPECT_FALSE(res.value());
}

TEST_F(AdapterTest, Double) {
  result<double> res;
  auto adapter = detail::result_traits<result<double>>::adapt(res);

  auto msg = make_simple_msg(resp3::type3::doublean, "3.14");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(res.has_value());
  EXPECT_DOUBLE_EQ(res.value(), 3.14);
}

TEST_F(AdapterTest, SimpleError) {
  result<int> res;
  auto adapter = detail::result_traits<result<int>>::adapt(res);

  auto msg = make_error_msg(resp3::type3::simple_error, "ERR something went wrong");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error().data_type, resp3::type3::simple_error);
  EXPECT_EQ(res.error().diagnostic, "ERR something went wrong");
}

TEST_F(AdapterTest, BlobError) {
  result<std::string> res;
  auto adapter = detail::result_traits<result<std::string>>::adapt(res);

  auto msg = make_error_msg(resp3::type3::blob_error, "detailed error message");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error().data_type, resp3::type3::blob_error);
  EXPECT_EQ(res.error().diagnostic, "detailed error message");
}

TEST_F(AdapterTest, Null) {
  result<int> res;
  auto adapter = detail::result_traits<result<int>>::adapt(res);

  auto msg = make_error_msg(resp3::type3::null, "");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error().data_type, resp3::type3::null);
}

TEST_F(AdapterTest, OptionalWithValue) {
  result<std::optional<int>> res;
  auto adapter = detail::result_traits<result<std::optional<int>>>::adapt(res);

  auto msg = make_simple_msg(resp3::type3::number, "42");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(res.has_value());
  EXPECT_TRUE(res.value().has_value());
  EXPECT_EQ(res.value().value(), 42);
}

TEST_F(AdapterTest, OptionalWithNull) {
  result<std::optional<int>> res;
  auto adapter = detail::result_traits<result<std::optional<int>>>::adapt(res);

  auto msg = make_simple_msg(resp3::type3::null, "");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_TRUE(res.has_value());
  EXPECT_FALSE(res.value().has_value());
}

TEST_F(AdapterTest, OptionalError) {
  result<std::optional<std::string>> res;
  auto adapter = detail::result_traits<result<std::optional<std::string>>>::adapt(res);

  auto msg = make_error_msg(resp3::type3::simple_error, "ERR failed");
  std::error_code ec;
  adapter.on_msg(msg, ec);

  EXPECT_FALSE(ec);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error().data_type, resp3::type3::simple_error);
  EXPECT_EQ(res.error().diagnostic, "ERR failed");
}
