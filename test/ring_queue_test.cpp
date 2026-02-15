#include <gtest/gtest.h>

#include <rediscoro/detail/ring_queue.hpp>

#include <memory>

TEST(ring_queue_test, wraparound_and_growth_preserves_order) {
  rediscoro::detail::ring_queue<int> q;

  for (int i = 0; i < 16; ++i) {
    q.push_back(i);
  }

  for (int i = 0; i < 10; ++i) {
    ASSERT_FALSE(q.empty());
    EXPECT_EQ(q.front(), i);
    q.pop_front();
  }

  for (int i = 16; i < 48; ++i) {
    q.push_back(i);
  }

  for (int want = 10; want < 48; ++want) {
    ASSERT_FALSE(q.empty());
    EXPECT_EQ(q.front(), want);
    q.pop_front();
  }

  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.size(), 0u);
}

TEST(ring_queue_test, move_only_type_stability) {
  rediscoro::detail::ring_queue<std::unique_ptr<int>> q;

  q.push_back(std::make_unique<int>(1));
  q.push_back(std::make_unique<int>(2));
  q.push_back(std::make_unique<int>(3));

  ASSERT_EQ(*q.front(), 1);
  q.pop_front();

  q.push_back(std::make_unique<int>(4));
  q.push_back(std::make_unique<int>(5));

  ASSERT_EQ(*q.front(), 2);
  q.pop_front();
  ASSERT_EQ(*q.front(), 3);
  q.pop_front();
  ASSERT_EQ(*q.front(), 4);
  q.pop_front();
  ASSERT_EQ(*q.front(), 5);
  q.pop_front();

  EXPECT_TRUE(q.empty());
}

TEST(ring_queue_test, move_assign_from_other_preserves_order) {
  rediscoro::detail::ring_queue<int> a;
  a.push_back(1);
  a.push_back(2);
  a.push_back(3);

  rediscoro::detail::ring_queue<int> moved{std::move(a)};
  ASSERT_EQ(moved.size(), 3u);
  EXPECT_EQ(moved.front(), 1);
  moved.pop_front();
  EXPECT_EQ(moved.front(), 2);
  moved.pop_front();
  EXPECT_EQ(moved.front(), 3);
  moved.pop_front();
  EXPECT_TRUE(moved.empty());

  moved.push_back(7);
  moved.push_back(8);

  rediscoro::detail::ring_queue<int> b;
  b.push_back(100);
  b = std::move(moved);

  ASSERT_EQ(b.size(), 2u);
  EXPECT_EQ(b.front(), 7);
  b.pop_front();
  EXPECT_EQ(b.front(), 8);
  b.pop_front();
  EXPECT_TRUE(b.empty());
}
