#include <rediscoro/response.hpp>

#include <gtest/gtest.h>

#include <cstddef>

namespace rediscoro::resp3 {

TEST(response, preserves_order_and_detects_redis_error) {
  rediscoro::response resp;

  resp.push_message(message{integer{1}});
  resp.push_message(message{simple_error{"ERR wrongtype"}});

  ASSERT_EQ(resp.size(), 2u);

  EXPECT_TRUE(resp[0].ok());
  EXPECT_FALSE(resp[0].is_redis_error());

  EXPECT_FALSE(resp[1].ok());
  EXPECT_TRUE(resp[1].is_redis_error());
  EXPECT_EQ(resp[1].error().as_redis_error().message, "ERR wrongtype");
}

TEST(response, adapt_as_returns_adapter_error) {
  rediscoro::response_item item{message{simple_string{"OK"}}};
  auto r = item.adapt_as<std::int64_t>();
  ASSERT_FALSE(r.has_value());
  EXPECT_TRUE(r.error().is_adapter_error());
}

}  // namespace rediscoro::resp3


