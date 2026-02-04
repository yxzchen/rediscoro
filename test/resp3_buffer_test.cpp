#include <gtest/gtest.h>
#include <cstring>
#include <rediscoro/resp3/buffer.hpp>

using namespace rediscoro::resp3;

namespace {

TEST(resp3_buffer_test, default_construction) {
  buffer buf;
  EXPECT_EQ(buf.size(), 0);
  EXPECT_TRUE(buf.data().empty());
}

TEST(resp3_buffer_test, construction_with_capacity) {
  buffer buf(8192);
  EXPECT_EQ(buf.size(), 0);

  auto writable = buf.prepare();
  EXPECT_GE(writable.size(), 8192);
}

TEST(resp3_buffer_test, prepare_and_commit) {
  buffer buf;

  // Prepare writable space
  auto writable = buf.prepare(100);
  EXPECT_GE(writable.size(), 100);

  // Write some data
  const char* test_data = "hello world";
  std::memcpy(writable.data(), test_data, 11);

  // Before commit, size should be 0
  EXPECT_EQ(buf.size(), 0);

  // Commit the written data
  buf.commit(11);
  EXPECT_EQ(buf.size(), 11);
  EXPECT_EQ(buf.data(), "hello world");
}

TEST(resp3_buffer_test, multiple_prepare_commit_cycles) {
  buffer buf;

  // First write
  auto w1 = buf.prepare(10);
  std::memcpy(w1.data(), "first", 5);
  buf.commit(5);

  // Second write
  auto w2 = buf.prepare(10);
  std::memcpy(w2.data(), "second", 6);
  buf.commit(6);

  EXPECT_EQ(buf.size(), 11);
  EXPECT_EQ(buf.data(), "firstsecond");
}

TEST(resp3_buffer_test, consume_data) {
  buffer buf;

  auto writable = buf.prepare(20);
  std::memcpy(writable.data(), "hello world", 11);
  buf.commit(11);

  EXPECT_EQ(buf.size(), 11);
  EXPECT_EQ(buf.data(), "hello world");

  // Consume first 6 bytes
  buf.consume(6);
  EXPECT_EQ(buf.size(), 5);
  EXPECT_EQ(buf.data(), "world");

  // Consume remaining
  buf.consume(5);
  EXPECT_EQ(buf.size(), 0);
  EXPECT_TRUE(buf.data().empty());
}

TEST(resp3_buffer_test, reset) {
  buffer buf;

  auto writable = buf.prepare(20);
  std::memcpy(writable.data(), "test data", 9);
  buf.commit(9);

  EXPECT_EQ(buf.size(), 9);

  buf.reset();
  EXPECT_EQ(buf.size(), 0);
  EXPECT_TRUE(buf.data().empty());

  // Should be able to write again after reset
  auto w2 = buf.prepare(10);
  std::memcpy(w2.data(), "new", 3);
  buf.commit(3);
  EXPECT_EQ(buf.size(), 3);
  EXPECT_EQ(buf.data(), "new");
}

TEST(resp3_buffer_test, manual_compact) {
  buffer buf(128);

  // Write some data
  auto w1 = buf.prepare(50);
  std::memcpy(w1.data(), "0123456789abcdefghij", 20);
  buf.commit(20);

  // Consume half
  buf.consume(10);
  EXPECT_EQ(buf.size(), 10);
  EXPECT_EQ(buf.data(), "abcdefghij");

  // Compact manually
  buf.compact();
  EXPECT_EQ(buf.size(), 10);
  EXPECT_EQ(buf.data(), "abcdefghij");

  // Should be able to write more after compact
  auto w2 = buf.prepare(10);
  std::memcpy(w2.data(), "KLMNO", 5);
  buf.commit(5);

  EXPECT_EQ(buf.size(), 15);
  EXPECT_EQ(buf.data(), "abcdefghijKLMNO");
}

TEST(resp3_buffer_test, auto_compact_on_consume) {
  buffer buf(128);

  // Fill buffer significantly
  auto w1 = buf.prepare(100);
  for (int i = 0; i < 100; ++i) {
    w1[i] = 'A' + (i % 26);
  }
  buf.commit(100);

  // Consume most of it (should trigger auto-compact)
  buf.consume(90);
  EXPECT_EQ(buf.size(), 10);

  // After auto-compact, the remaining data should still be valid
  auto remaining_data = buf.data();
  EXPECT_EQ(remaining_data.size(), 10);
}

TEST(resp3_buffer_test, buffer_growth) {
  buffer buf(64);

  // Request large space that exceeds initial capacity
  auto writable = buf.prepare(200);
  EXPECT_GE(writable.size(), 200);

  // Fill it
  for (std::size_t i = 0; i < 200; ++i) {
    writable[i] = 'X';
  }
  buf.commit(200);

  EXPECT_EQ(buf.size(), 200);
  auto data = buf.data();
  EXPECT_EQ(data.size(), 200);
  EXPECT_EQ(data.front(), 'X');
  EXPECT_EQ(data.back(), 'X');
}

TEST(resp3_buffer_test, alternating_operations) {
  buffer buf;

  // Write, consume, write, consume pattern
  auto w1 = buf.prepare(10);
  std::memcpy(w1.data(), "AAA", 3);
  buf.commit(3);
  EXPECT_EQ(buf.data(), "AAA");

  buf.consume(1);
  EXPECT_EQ(buf.data(), "AA");

  auto w2 = buf.prepare(10);
  std::memcpy(w2.data(), "BBB", 3);
  buf.commit(3);
  EXPECT_EQ(buf.data(), "AABBB");

  buf.consume(2);
  EXPECT_EQ(buf.data(), "BBB");

  buf.consume(3);
  EXPECT_EQ(buf.size(), 0);
}

TEST(resp3_buffer_test, compact_with_no_consumed_data) {
  buffer buf;

  auto writable = buf.prepare(20);
  std::memcpy(writable.data(), "no consume yet", 14);
  buf.commit(14);

  // Compact without consuming anything
  buf.compact();

  // Data should remain unchanged
  EXPECT_EQ(buf.size(), 14);
  EXPECT_EQ(buf.data(), "no consume yet");
}

TEST(resp3_buffer_test, prepare_with_zero_size) {
  buffer buf;

  auto writable = buf.prepare(0);
  // Should return at least default size
  EXPECT_GT(writable.size(), 0);
}

}  // namespace
