#include <xz/redis/request.hpp>
#include <gtest/gtest.h>

#include <deque>
#include <list>
#include <vector>

class RequestTest : public ::testing::Test {
 protected:
  void SetUp() override {}
  void TearDown() override {}

  xz::redis::request req;
};

// === Basic Push Tests ===

TEST_F(RequestTest, PushSimpleCommandNoArgs) {
  req.push("PING");

  EXPECT_EQ(req.expected_responses(), 1);
  EXPECT_FALSE(req.payload().empty());

  // Should be: *1\r\n$4\r\nPING\r\n
  auto payload = req.payload();
  EXPECT_EQ(payload, "*1\r\n$4\r\nPING\r\n");
}

TEST_F(RequestTest, PushCommandWithOneArg) {
  req.push("GET", "mykey");

  EXPECT_EQ(req.expected_responses(), 1);

  // Should be: *2\r\n$3\r\nGET\r\n$5\r\nmykey\r\n
  auto payload = req.payload();
  EXPECT_EQ(payload, "*2\r\n$3\r\nGET\r\n$5\r\nmykey\r\n");
}

TEST_F(RequestTest, PushCommandWithMultipleArgs) {
  req.push("SET", "mykey", "myvalue");

  EXPECT_EQ(req.expected_responses(), 1);

  // Should be: *3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$7\r\nmyvalue\r\n
  auto payload = req.payload();
  EXPECT_EQ(payload, "*3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$7\r\nmyvalue\r\n");
}

TEST_F(RequestTest, PushCommandWithIntegerArg) {
  req.push("EXPIRE", "mykey", 300);

  EXPECT_EQ(req.expected_responses(), 1);

  // Should be: *3\r\n$6\r\nEXPIRE\r\n$5\r\nmykey\r\n$3\r\n300\r\n
  auto payload = req.payload();
  EXPECT_EQ(payload, "*3\r\n$6\r\nEXPIRE\r\n$5\r\nmykey\r\n$3\r\n300\r\n");
}

TEST_F(RequestTest, PushCommandWithMixedArgs) {
  req.push("ZADD", "myset", 1, "member1", 2, "member2");

  EXPECT_EQ(req.expected_responses(), 1);

  // Should be: *6\r\n$4\r\nZADD\r\n$5\r\nmyset\r\n$1\r\n1\r\n$7\r\nmember1\r\n$1\r\n2\r\n$7\r\nmember2\r\n
  auto payload = req.payload();
  EXPECT_EQ(payload, "*6\r\n$4\r\nZADD\r\n$5\r\nmyset\r\n$1\r\n1\r\n$7\r\nmember1\r\n$1\r\n2\r\n$7\r\nmember2\r\n");
}

// === Subscription Commands Tests ===
// These test different kinds of "push" - subscription commands that don't expect regular responses

TEST_F(RequestTest, PushSubscribeCommand) {
  req.push("SUBSCRIBE", "channel1");

  // SUBSCRIBE should NOT increment expected_responses (it uses push protocol)
  EXPECT_EQ(req.expected_responses(), 0);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*2\r\n$9\r\nSUBSCRIBE\r\n$8\r\nchannel1\r\n");
}

TEST_F(RequestTest, PushPSubscribeCommand) {
  req.push("PSUBSCRIBE", "pattern*");

  // PSUBSCRIBE should NOT increment expected_responses
  EXPECT_EQ(req.expected_responses(), 0);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*2\r\n$10\r\nPSUBSCRIBE\r\n$8\r\npattern*\r\n");
}

TEST_F(RequestTest, PushUnsubscribeCommand) {
  req.push("UNSUBSCRIBE", "channel1");

  // UNSUBSCRIBE should NOT increment expected_responses
  EXPECT_EQ(req.expected_responses(), 0);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*2\r\n$11\r\nUNSUBSCRIBE\r\n$8\r\nchannel1\r\n");
}

TEST_F(RequestTest, PushPUnsubscribeCommand) {
  req.push("PUNSUBSCRIBE", "pattern*");

  // PUNSUBSCRIBE should NOT increment expected_responses
  EXPECT_EQ(req.expected_responses(), 0);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*2\r\n$12\r\nPUNSUBSCRIBE\r\n$8\r\npattern*\r\n");
}

TEST_F(RequestTest, PushMultipleSubscriptionCommands) {
  req.push("SUBSCRIBE", "channel1");
  req.push("SUBSCRIBE", "channel2");
  req.push("PSUBSCRIBE", "pattern*");

  // None of these should increment expected_responses
  EXPECT_EQ(req.expected_responses(), 0);
}

// === Push Range Tests ===

TEST_F(RequestTest, PushRangeWithVector) {
  std::vector<std::string> values = {"value1", "value2", "value3"};
  req.push_range("RPUSH", "mylist", values);

  EXPECT_EQ(req.expected_responses(), 1);

  // Should be: *5\r\n$5\r\nRPUSH\r\n$6\r\nmylist\r\n$6\r\nvalue1\r\n$6\r\nvalue2\r\n$6\r\nvalue3\r\n
  auto payload = req.payload();
  EXPECT_EQ(payload, "*5\r\n$5\r\nRPUSH\r\n$6\r\nmylist\r\n$6\r\nvalue1\r\n$6\r\nvalue2\r\n$6\r\nvalue3\r\n");
}

TEST_F(RequestTest, PushRangeWithList) {
  std::list<std::string> values = {"a", "b", "c"};
  req.push_range("LPUSH", "mylist", values);

  EXPECT_EQ(req.expected_responses(), 1);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*5\r\n$5\r\nLPUSH\r\n$6\r\nmylist\r\n$1\r\na\r\n$1\r\nb\r\n$1\r\nc\r\n");
}

TEST_F(RequestTest, PushRangeWithDeque) {
  std::deque<int> values = {1, 2, 3};
  req.push_range("RPUSH", "mylist", values);

  EXPECT_EQ(req.expected_responses(), 1);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*5\r\n$5\r\nRPUSH\r\n$6\r\nmylist\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n");
}

TEST_F(RequestTest, PushRangeWithEmptyRange) {
  std::vector<std::string> values;
  req.push_range("RPUSH", "mylist", values);

  // Empty range should not add anything
  EXPECT_EQ(req.expected_responses(), 0);
  EXPECT_TRUE(req.payload().empty());
}

TEST_F(RequestTest, PushRangeWithIterators) {
  std::vector<std::string> values = {"x", "y", "z"};
  req.push_range("SADD", "myset", values.begin(), values.end());

  EXPECT_EQ(req.expected_responses(), 1);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*5\r\n$4\r\nSADD\r\n$5\r\nmyset\r\n$1\r\nx\r\n$1\r\ny\r\n$1\r\nz\r\n");
}

TEST_F(RequestTest, PushRangeWithoutKey) {
  std::vector<std::string> values = {"arg1", "arg2"};
  req.push_range("DEL", values);

  EXPECT_EQ(req.expected_responses(), 1);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*3\r\n$3\r\nDEL\r\n$4\r\narg1\r\n$4\r\narg2\r\n");
}

// === Multiple Commands Tests ===

TEST_F(RequestTest, MultipleCommands) {
  req.push("GET", "key1");
  req.push("SET", "key2", "value2");
  req.push("DEL", "key3");

  EXPECT_EQ(req.expected_responses(), 3);

  auto payload = req.payload();
  // Should contain all three commands
  EXPECT_TRUE(payload.find("GET") != std::string::npos);
  EXPECT_TRUE(payload.find("SET") != std::string::npos);
  EXPECT_TRUE(payload.find("DEL") != std::string::npos);
}

// === Clear and Reuse Tests ===

TEST_F(RequestTest, ClearRequest) {
  req.push("GET", "key1");
  req.push("SET", "key2", "value2");

  EXPECT_EQ(req.expected_responses(), 2);
  EXPECT_FALSE(req.payload().empty());

  req.clear();

  EXPECT_EQ(req.expected_responses(), 0);
  EXPECT_TRUE(req.payload().empty());
}

TEST_F(RequestTest, ReuseAfterClear) {
  req.push("GET", "key1");
  req.clear();
  req.push("SET", "key2", "value2");

  EXPECT_EQ(req.expected_responses(), 1);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*3\r\n$3\r\nSET\r\n$4\r\nkey2\r\n$6\r\nvalue2\r\n");
}

// === Reserve Tests ===

TEST_F(RequestTest, ReserveCapacity) {
  req.reserve(1024);
  req.push("GET", "key1");

  EXPECT_EQ(req.expected_responses(), 1);
  EXPECT_FALSE(req.payload().empty());
}

// === Edge Cases ===

TEST_F(RequestTest, PushWithEmptyStringArg) {
  req.push("SET", "key", "");

  EXPECT_EQ(req.expected_responses(), 1);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$0\r\n\r\n");
}

TEST_F(RequestTest, PushWithZeroIntegerArg) {
  req.push("EXPIRE", "key", 0);

  EXPECT_EQ(req.expected_responses(), 1);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*3\r\n$6\r\nEXPIRE\r\n$3\r\nkey\r\n$1\r\n0\r\n");
}

TEST_F(RequestTest, PushWithNegativeInteger) {
  req.push("INCRBY", "counter", -5);

  EXPECT_EQ(req.expected_responses(), 1);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*3\r\n$6\r\nINCRBY\r\n$7\r\ncounter\r\n$2\r\n-5\r\n");
}

TEST_F(RequestTest, PushSubscribeWithMultipleChannels) {
  req.push("SUBSCRIBE", "ch1", "ch2", "ch3");

  // SUBSCRIBE with multiple channels should still not increment
  EXPECT_EQ(req.expected_responses(), 0);

  auto payload = req.payload();
  EXPECT_EQ(payload, "*4\r\n$9\r\nSUBSCRIBE\r\n$3\r\nch1\r\n$3\r\nch2\r\n$3\r\nch3\r\n");
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
