#include <rediscoro/detail/response_builder.hpp>
#include <rediscoro/response.hpp>

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
  EXPECT_EQ(r1.error().code, rediscoro::server_errc::redis_error);
  EXPECT_EQ(r1.error().detail, "ERR wrongtype");
}

TEST(response, adapt_as_returns_adapter_error) {
  rediscoro::detail::response_builder<std::int64_t> b;
  b.accept(message{simple_string{"OK"}});
  auto resp = b.take_results();
  auto& r = resp.get<0>();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code.category().name(), std::string{"rediscoro.adapter"});
  EXPECT_FALSE(r.error().detail.empty());
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
  EXPECT_EQ(resp[1].error().code, rediscoro::server_errc::redis_error);

  EXPECT_FALSE(resp[2].has_value());
  EXPECT_EQ(resp[2].error().code.category().name(), std::string{"rediscoro.adapter"});
}

}  // namespace rediscoro::resp3
