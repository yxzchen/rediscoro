#include <rediscoro/adapter/adapt.hpp>
#include <rediscoro/resp3/message.hpp>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rediscoro::resp3 {

TEST(resp3_adapter, scalar_string_like) {
  message m{simple_string{"OK"}};
  auto r = rediscoro::adapter::adapt<std::string>(m);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, "OK");
}

TEST(resp3_adapter, optional_null) {
  message m{null{}};
  auto r = rediscoro::adapter::adapt<std::optional<std::int64_t>>(m);
  ASSERT_TRUE(r.has_value());
  EXPECT_FALSE(r->has_value());
}

TEST(resp3_adapter, vector_of_int) {
  message m{array{{message{integer{1}}, message{integer{2}}, message{integer{3}}}}};
  auto r = rediscoro::adapter::adapt<std::vector<int>>(m);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ((*r).size(), 3u);
  EXPECT_EQ((*r)[0], 1);
  EXPECT_EQ((*r)[2], 3);
}

TEST(resp3_adapter, map_string_to_int) {
  message m{map{{{message{simple_string{"a"}}, message{integer{1}}},
                 {message{simple_string{"b"}}, message{integer{2}}}}}};
  auto r = rediscoro::adapter::adapt<std::unordered_map<std::string, int>>(m);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ((*r).at("a"), 1);
  EXPECT_EQ((*r).at("b"), 2);
}

TEST(resp3_adapter, map_value_error_includes_key_path) {
  message m{map{{{message{simple_string{"a"}}, message{simple_string{"oops"}}}}}};
  auto r = rediscoro::adapter::adapt<std::unordered_map<std::string, int>>(m);
  ASSERT_FALSE(r.has_value());
  ASSERT_FALSE(r.error().path.empty());
  EXPECT_TRUE(std::holds_alternative<rediscoro::adapter::path_key>(r.error().path[0]));
  EXPECT_EQ(std::get<rediscoro::adapter::path_key>(r.error().path[0]).key, "a");
}

TEST(resp3_adapter, ignore_always_ok) {
  message m{simple_error{"ERR"}};
  auto r = rediscoro::adapter::adapt<rediscoro::ignore_t>(m);
  EXPECT_TRUE(r.has_value());
}

TEST(resp3_adapter, std_array_size_mismatch) {
  message m{array{{message{integer{1}}, message{integer{2}}}}};
  auto r = rediscoro::adapter::adapt<std::array<int, 3>>(m);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, rediscoro::adapter_errc::size_mismatch);
}

TEST(resp3_adapter, uint64_accepts_non_negative_int64_and_rejects_negative) {
  message ok{integer{std::numeric_limits<std::int64_t>::max()}};
  auto r_ok = rediscoro::adapter::adapt<std::uint64_t>(ok);
  ASSERT_TRUE(r_ok.has_value());
  EXPECT_EQ(*r_ok, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));

  message neg{integer{-1}};
  auto r_neg = rediscoro::adapter::adapt<std::uint64_t>(neg);
  ASSERT_FALSE(r_neg.has_value());
  EXPECT_EQ(r_neg.error().kind, rediscoro::adapter_errc::value_out_of_range);
}

TEST(resp3_adapter, unsigned_narrow_type_checks_upper_bound) {
  message m{integer{256}};
  auto r = rediscoro::adapter::adapt<std::uint8_t>(m);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, rediscoro::adapter_errc::value_out_of_range);
}

TEST(resp3_adapter, map_duplicate_key_returns_error) {
  message m{map{{{message{simple_string{"a"}}, message{integer{1}}},
                 {message{simple_string{"a"}}, message{integer{2}}}}}};
  auto r = rediscoro::adapter::adapt<std::unordered_map<std::string, int>>(m);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, rediscoro::adapter_errc::duplicate_key);
  ASSERT_FALSE(r.error().path.empty());
  EXPECT_TRUE(std::holds_alternative<rediscoro::adapter::path_key>(r.error().path[0]));
  EXPECT_EQ(std::get<rediscoro::adapter::path_key>(r.error().path[0]).key, "a");
}

}  // namespace rediscoro::resp3
