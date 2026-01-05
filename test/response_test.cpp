#include <rediscoro/response.hpp>
#include <rediscoro/response_builder.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace rediscoro::resp3 {

TEST(response, preserves_order_and_detects_redis_error) {
  rediscoro::response_builder<std::int64_t, std::int64_t> b;
  b.accept(message{integer{1}});
  b.accept(message{simple_error{"ERR wrongtype"}});
  ASSERT_TRUE(b.done());
  auto resp = b.take_results();

  auto& r0 = resp.get<0>();
  ASSERT_TRUE(r0.has_value());
  EXPECT_EQ(*r0, 1);

  auto& r1 = resp.get<1>();
  ASSERT_FALSE(r1.has_value());
  EXPECT_TRUE(r1.error().is_redis_error());
  EXPECT_EQ(r1.error().as_redis_error().message, "ERR wrongtype");
}

TEST(response, adapt_as_returns_adapter_error) {
  rediscoro::response_builder<std::int64_t> b;
  b.accept(message{simple_string{"OK"}});
  auto resp = b.take_results();
  auto& r = resp.get<0>();
  ASSERT_FALSE(r.has_value());
  EXPECT_TRUE(r.error().is_adapter_error());
}

}  // namespace rediscoro::resp3


