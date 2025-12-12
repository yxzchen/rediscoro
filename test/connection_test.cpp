#include <xz/io/io_context.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/detail/connection.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/response.hpp>

#include <gtest/gtest.h>
#include <iostream>

using namespace xz::redis;
using namespace xz::io;
namespace redis_detail = xz::redis::detail;

class ConnectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.connect_timeout = std::chrono::milliseconds{5000};
    cfg.request_timeout = std::chrono::milliseconds{5000};
  }

  config cfg;
};

TEST_F(ConnectionTest, ConnectAndPing) {
  std::cout << "Starting ConnectAndPing test\n";
  io_context ctx;
  std::cout << "Created io_context\n";

  auto test_task = [&]() -> task<void> {
    std::cout << "In test_task coroutine\n";
    redis_detail::connection conn{ctx, cfg};
    std::cout << "Created connection\n";

    co_await conn.connect();
    std::cout << "Connected\n";
    EXPECT_TRUE(conn.is_connected());

    request req;
    req.push("PING");
    std::cout << "Created request\n";
    response<std::string> resp;

    co_await conn.exec(req, resp);
    std::cout << "Executed request\n";

    EXPECT_TRUE(std::get<0>(resp).has_value());
    EXPECT_EQ(std::get<0>(resp).value(), "PONG");

    conn.close();
    std::cout << "Closed connection\n";
  };

  std::cout << "Creating task\n";
  auto t = test_task();
  std::cout << "Resuming task\n";
  t.resume();
  std::cout << "Running io_context\n";
  ctx.run();
  std::cout << "Test complete\n";
}

TEST_F(ConnectionTest, PingWithMessage) {
  io_context ctx;

  auto test_task = [&]() -> task<void> {
    redis_detail::connection conn{ctx, cfg};
    co_await conn.connect();

    request req;
    req.push("PING", "Hello world");
    response<std::string> resp;

    co_await conn.exec(req, resp);

    EXPECT_TRUE(std::get<0>(resp).has_value());
    EXPECT_EQ(std::get<0>(resp).value(), "Hello world");

    conn.close();
  };

  auto t = test_task();
  t.resume();
  ctx.run();
}

TEST_F(ConnectionTest, SetAndGet) {
  io_context ctx;

  auto test_task = [&]() -> task<void> {
    redis_detail::connection conn{ctx, cfg};
    co_await conn.connect();

    request set_req;
    set_req.push("SET", "test_key", "test_value");
    response<std::string> set_resp;

    co_await conn.exec(set_req, set_resp);
    EXPECT_TRUE(std::get<0>(set_resp).has_value());
    EXPECT_EQ(std::get<0>(set_resp).value(), "OK");

    request get_req;
    get_req.push("GET", "test_key");
    response<std::string> get_resp;

    co_await conn.exec(get_req, get_resp);
    EXPECT_TRUE(std::get<0>(get_resp).has_value());
    EXPECT_EQ(std::get<0>(get_resp).value(), "test_value");

    request del_req;
    del_req.push("DEL", "test_key");
    response<int> del_resp;

    co_await conn.exec(del_req, del_resp);
    EXPECT_TRUE(std::get<0>(del_resp).has_value());
    EXPECT_EQ(std::get<0>(del_resp).value(), 1);

    conn.close();
  };

  auto t = test_task();
  t.resume();
  ctx.run();
}

TEST_F(ConnectionTest, MultipleCommands) {
  io_context ctx;

  auto test_task = [&]() -> task<void> {
    redis_detail::connection conn{ctx, cfg};
    co_await conn.connect();

    request req;
    req.push("PING");
    req.push("PING", "test");
    req.push("ECHO", "hello");

    response<std::string, std::string, std::string> resp;

    co_await conn.exec(req, resp);

    EXPECT_TRUE(std::get<0>(resp).has_value());
    EXPECT_EQ(std::get<0>(resp).value(), "PONG");

    EXPECT_TRUE(std::get<1>(resp).has_value());
    EXPECT_EQ(std::get<1>(resp).value(), "test");

    EXPECT_TRUE(std::get<2>(resp).has_value());
    EXPECT_EQ(std::get<2>(resp).value(), "hello");

    conn.close();
  };

  auto t = test_task();
  t.resume();
  ctx.run();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
