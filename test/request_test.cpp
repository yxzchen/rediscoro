#include <rediscoro/request.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace rediscoro {

TEST(request, encode_ping) {
  request r{"PING"};
  EXPECT_EQ(r.wire(), "*1\r\n$4\r\nPING\r\n");
  EXPECT_EQ(r.command_count(), 1u);
}

TEST(request, encode_get_key) {
  request r{"GET", "mykey"};
  EXPECT_EQ(r.wire(), "*2\r\n$3\r\nGET\r\n$5\r\nmykey\r\n");
  EXPECT_EQ(r.command_count(), 1u);
}

TEST(request, encode_from_argv_span) {
  std::array<std::string_view, 3> argv{"SET", "k", "v"};
  request r{std::span<const std::string_view>(argv)};
  EXPECT_EQ(r.wire(), "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n");
  EXPECT_EQ(r.command_count(), 1u);
}

TEST(request, encode_multiple_commands_appends_and_counts) {
  request r;
  r.push({"PING"});
  r.push({"GET", "k"});
  EXPECT_EQ(r.command_count(), 2u);
  EXPECT_EQ(r.wire(), "*1\r\n$4\r\nPING\r\n*2\r\n$3\r\nGET\r\n$1\r\nk\r\n");
}

TEST(request, encode_binary_argument_with_nul) {
  std::string bin;
  bin.push_back('a');
  bin.push_back('\0');
  bin.push_back('b');

  request r;
  r.push({"ECHO", std::string_view{bin}});

  const auto w = r.wire();
  std::string expected;
  expected.append("*2\r\n$4\r\nECHO\r\n$3\r\n");
  expected.push_back('a');
  expected.push_back('\0');
  expected.push_back('b');
  expected.append("\r\n");
  EXPECT_EQ(w.size(), expected.size());
  EXPECT_EQ(w, expected);
  EXPECT_EQ(r.command_count(), 1u);
}

}  // namespace rediscoro
