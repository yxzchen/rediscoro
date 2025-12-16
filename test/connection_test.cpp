#include <xz/io/io_context.hpp>
#include <xz/io/work_guard.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/connection.hpp>
#include <xz/redis/request.hpp>
#include <xz/redis/response.hpp>

#include <gtest/gtest.h>

using namespace xz::redis;
using namespace xz::io;

class ConnectionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // cfg.host = "153.3.238.127";
    // cfg.port = 80;
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.connect_timeout = std::chrono::milliseconds{1000};
    cfg.request_timeout = std::chrono::milliseconds{1000};
    // Exercise handshake steps against local Redis.
    cfg.database = 1;
    cfg.client_name = std::string{"redisxz-test"};
  }

  config cfg;
};

TEST_F(ConnectionTest, RunBasic) {
  io_context ctx;
  connection conn{ctx, cfg};
  
  // Keep io_context alive during the test
  // auto guard = std::make_shared<work_guard<io_context>>(ctx);

  co_spawn(
      ctx,
      [&]() -> awaitable<void> {
        co_await conn.run();
        EXPECT_TRUE(conn.is_running());

        // Verify handshake applied client name.
        request req;
        req.push("CLIENT", "GETNAME");
        response<std::optional<std::string>> name;
        co_await conn.execute(req, name);
        if (!std::get<0>(name).has_value()) {
          ADD_FAILURE() << "CLIENT GETNAME failed: " << std::get<0>(name).error().msg;
          co_return;
        }
        if (!std::get<0>(name).value().has_value()) {
          ADD_FAILURE() << "CLIENT GETNAME returned null (name not set)";
          co_return;
        }
        EXPECT_EQ(std::get<0>(name).value().value(), "redisxz-test");
      },
      [&](std::exception_ptr eptr) mutable {
        // Cleanup
        conn.stop();
        
        // Check for exceptions
        if (eptr) {
          try {
            std::rethrow_exception(eptr);
          } catch (std::system_error const& e) {
            ADD_FAILURE() << "System error: " << e.what();
          } catch (std::exception const& e) {
            ADD_FAILURE() << "Exception: " << e.what();
          } catch (...) {
            ADD_FAILURE() << "Unknown exception";
          }
        }
      });

  ctx.run();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
