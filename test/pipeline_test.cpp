#include <xz/redis/detail/pipeline.hpp>
#include <gtest/gtest.h>
#include <thread>

using namespace xz::redis;
using namespace std::chrono_literals;

class PipelineTest : public ::testing::Test {};

TEST_F(PipelineTest, InitiallyEmpty) {
  pipeline p;
  EXPECT_TRUE(p.empty());
  EXPECT_EQ(p.size(), 0);
  EXPECT_EQ(p.front(), nullptr);
}

TEST_F(PipelineTest, PushOneRequest) {
  pipeline p;
  p.push(1, 1000ms);

  EXPECT_FALSE(p.empty());
  EXPECT_EQ(p.size(), 1);

  auto* req = p.front();
  ASSERT_NE(req, nullptr);
  EXPECT_EQ(req->expected_responses, 1);
}

TEST_F(PipelineTest, PushMultipleRequests) {
  pipeline p;
  p.push(1, 1000ms);
  p.push(2, 2000ms);
  p.push(3, 3000ms);

  EXPECT_EQ(p.size(), 3);

  auto* req = p.front();
  ASSERT_NE(req, nullptr);
  EXPECT_EQ(req->expected_responses, 1);
}

TEST_F(PipelineTest, PopRequest) {
  pipeline p;
  p.push(1, 1000ms);
  p.push(2, 2000ms);

  EXPECT_EQ(p.size(), 2);

  p.pop();
  EXPECT_EQ(p.size(), 1);

  auto* req = p.front();
  ASSERT_NE(req, nullptr);
  EXPECT_EQ(req->expected_responses, 2);

  p.pop();
  EXPECT_EQ(p.size(), 0);
  EXPECT_TRUE(p.empty());
  EXPECT_EQ(p.front(), nullptr);
}

TEST_F(PipelineTest, PopEmptyPipeline) {
  pipeline p;
  p.pop();
  EXPECT_TRUE(p.empty());
}

TEST_F(PipelineTest, Clear) {
  pipeline p;
  p.push(1, 1000ms);
  p.push(2, 2000ms);
  p.push(3, 3000ms);

  EXPECT_EQ(p.size(), 3);

  p.clear();
  EXPECT_TRUE(p.empty());
  EXPECT_EQ(p.size(), 0);
}

TEST_F(PipelineTest, FifoOrdering) {
  pipeline p;
  p.push(1, 100ms);
  p.push(2, 200ms);
  p.push(3, 300ms);

  EXPECT_EQ(p.front()->expected_responses, 1);
  p.pop();

  EXPECT_EQ(p.front()->expected_responses, 2);
  p.pop();

  EXPECT_EQ(p.front()->expected_responses, 3);
  p.pop();

  EXPECT_TRUE(p.empty());
}

TEST_F(PipelineTest, TimeoutDetection) {
  pipeline p;
  auto now = std::chrono::steady_clock::now();

  p.push(1, 50ms);
  p.push(2, 1000ms);

  auto timed_out = p.find_timed_out(now);
  EXPECT_FALSE(timed_out.has_value());

  std::this_thread::sleep_for(100ms);
  now = std::chrono::steady_clock::now();

  timed_out = p.find_timed_out(now);
  ASSERT_TRUE(timed_out.has_value());
  EXPECT_EQ(*timed_out, 0);
}

TEST_F(PipelineTest, NoTimeoutWhenEmpty) {
  pipeline p;
  auto now = std::chrono::steady_clock::now();

  auto timed_out = p.find_timed_out(now);
  EXPECT_FALSE(timed_out.has_value());
}

TEST_F(PipelineTest, NextDeadline) {
  pipeline p;

  auto deadline = p.next_deadline();
  EXPECT_FALSE(deadline.has_value());

  auto before = std::chrono::steady_clock::now();
  p.push(1, 100ms);
  auto after = std::chrono::steady_clock::now();

  deadline = p.next_deadline();
  ASSERT_TRUE(deadline.has_value());
  EXPECT_GE(*deadline, before + 100ms);
  EXPECT_LE(*deadline, after + 100ms);
}

TEST_F(PipelineTest, NextDeadlineIsFirstRequest) {
  pipeline p;
  p.push(1, 1000ms);
  p.push(2, 500ms);

  auto deadline1 = p.next_deadline();
  p.pop();
  auto deadline2 = p.next_deadline();

  EXPECT_NE(deadline1, deadline2);
}

TEST_F(PipelineTest, MultipleResponsesPerRequest) {
  pipeline p;
  p.push(5, 1000ms);

  auto* req = p.front();
  ASSERT_NE(req, nullptr);
  EXPECT_EQ(req->expected_responses, 5);
}

TEST_F(PipelineTest, PipelinedRequests) {
  pipeline p;
  for (int i = 0; i < 100; ++i) {
    p.push(1, 5000ms);
  }

  EXPECT_EQ(p.size(), 100);

  for (int i = 0; i < 100; ++i) {
    EXPECT_FALSE(p.empty());
    p.pop();
  }

  EXPECT_TRUE(p.empty());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
