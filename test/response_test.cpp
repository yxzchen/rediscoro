#include <rediscoro/response.hpp>
#include <rediscoro/detail/response_builder.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace rediscoro::resp3 {

TEST(response, preserves_order_and_detects_redis_error) {
  rediscoro::detail::response_builder<std::int64_t, std::int64_t> b;
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
  rediscoro::detail::response_builder<std::int64_t> b;
  b.accept(message{simple_string{"OK"}});
  auto resp = b.take_results();
  auto& r = resp.get<0>();
  ASSERT_FALSE(r.has_value());
  EXPECT_TRUE(r.error().is_adapter_error());
}

TEST(response_dynamic, fills_n_results_in_order) {
  rediscoro::detail::dynamic_response_builder<std::string> b{3};

  b.accept(message{bulk_string{"a"}});
  b.accept(message{simple_error{"ERR wrongtype"}});
  b.accept(message{integer{1}});  // adapter error when expecting string

  ASSERT_TRUE(b.done());
  auto resp = b.take_results();

  ASSERT_EQ(resp.size(), 3u);

  EXPECT_TRUE(resp[0].has_value());
  EXPECT_EQ(*resp[0], "a");

  EXPECT_FALSE(resp[1].has_value());
  EXPECT_TRUE(resp[1].error().is_redis_error());

  EXPECT_FALSE(resp[2].has_value());
  EXPECT_TRUE(resp[2].error().is_adapter_error());
}

}  // namespace rediscoro::resp3


