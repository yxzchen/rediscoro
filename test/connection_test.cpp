#include <xz/io/io_context.hpp>
#include <xz/io/work_guard.hpp>
#include <xz/io/co_spawn.hpp>
#include <xz/redis/config.hpp>
#include <xz/redis/connection.hpp>

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
  }

  config cfg;
};

TEST_F(ConnectionTest, RunBasic) {
  io_context ctx;
  connection conn{ctx, cfg};
  
  // Keep io_context alive during the test
  auto guard = std::make_shared<work_guard<io_context>>(ctx);

  co_spawn(
      ctx,
      [&]() -> awaitable<void> {
        co_await conn.run();
        EXPECT_TRUE(conn.is_running());
      },
      [&, guard](std::exception_ptr eptr) mutable {
        // Cleanup
        conn.stop();
        guard.reset();  // Release work_guard
        
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
        
        // Stop io_context to exit the event loop
        ctx.stop();
      });

  ctx.run();
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
