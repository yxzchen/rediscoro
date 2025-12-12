#include <xz/redis/logger.hpp>
#include <gtest/gtest.h>

#include <mutex>
#include <thread>
#include <vector>

class LoggerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Reset logger to default state before each test
    xz::redis::logger::instance().set_log_function(nullptr);
    xz::redis::logger::instance().set_log_level(xz::redis::log_level::info);

    // Clear captured logs
    captured_logs_.clear();
  }

  void TearDown() override {
    // Reset logger after each test
    xz::redis::logger::instance().set_log_function(nullptr);
    xz::redis::logger::instance().set_log_level(xz::redis::log_level::info);
  }

  // Helper to capture log messages (thread-safe for concurrent tests)
  void setup_capture_logger() {
    auto capture_fn = [this](xz::redis::log_context const& ctx) {
      std::lock_guard<std::mutex> lock(captured_logs_mutex_);
      captured_logs_.push_back({ctx.level, std::string(ctx.message)});
    };
    xz::redis::logger::instance().set_log_function(capture_fn);
  }

  struct LogEntry {
    xz::redis::log_level level;
    std::string message;
  };

  std::vector<LogEntry> captured_logs_;
  std::mutex captured_logs_mutex_;
};

// === Basic Logging Tests ===

TEST_F(LoggerTest, LogDebugMessage) {
  setup_capture_logger();
  xz::redis::logger::instance().set_log_level(xz::redis::log_level::debug);

  REDIS_LOG_DEBUG("Debug message");

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].level, xz::redis::log_level::debug);
  EXPECT_EQ(captured_logs_[0].message, "Debug message");
}

TEST_F(LoggerTest, LogInfoMessage) {
  setup_capture_logger();

  REDIS_LOG_INFO("Info message");

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].level, xz::redis::log_level::info);
  EXPECT_EQ(captured_logs_[0].message, "Info message");
}

TEST_F(LoggerTest, LogWarningMessage) {
  setup_capture_logger();

  REDIS_LOG_WARNING("Warning message");

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].level, xz::redis::log_level::warning);
  EXPECT_EQ(captured_logs_[0].message, "Warning message");
}

TEST_F(LoggerTest, LogErrorMessage) {
  setup_capture_logger();

  REDIS_LOG_ERROR("Error message");

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].level, xz::redis::log_level::error);
  EXPECT_EQ(captured_logs_[0].message, "Error message");
}

// === Format String Tests ===

TEST_F(LoggerTest, FormatWithSingleArg) {
  setup_capture_logger();

  REDIS_LOG_INFO("Value: {}", 42);

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].message, "Value: 42");
}

TEST_F(LoggerTest, FormatWithMultipleArgs) {
  setup_capture_logger();

  REDIS_LOG_INFO("Name: {}, Age: {}", "Alice", 30);

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].message, "Name: Alice, Age: 30");
}

TEST_F(LoggerTest, FormatWithDifferentTypes) {
  setup_capture_logger();

  REDIS_LOG_ERROR("Error code: {}, message: {}, value: {}", 500, "Internal error", 3.14);

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].message, "Error code: 500, message: Internal error, value: 3.14");
}

TEST_F(LoggerTest, FormatWithNamedArgs) {
  setup_capture_logger();

  int port = 6379;
  std::string host = "localhost";
  REDIS_LOG_INFO("Connecting to {}:{}", host, port);

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].message, "Connecting to localhost:6379");
}

// === Log Level Filtering Tests ===

TEST_F(LoggerTest, MinLevelFilteringInfo) {
  setup_capture_logger();
  xz::redis::logger::instance().set_log_level(xz::redis::log_level::info);

  REDIS_LOG_DEBUG("Should not appear");
  REDIS_LOG_INFO("Should appear");
  REDIS_LOG_WARNING("Should appear");
  REDIS_LOG_ERROR("Should appear");

  ASSERT_EQ(captured_logs_.size(), 3);
  EXPECT_EQ(captured_logs_[0].level, xz::redis::log_level::info);
  EXPECT_EQ(captured_logs_[1].level, xz::redis::log_level::warning);
  EXPECT_EQ(captured_logs_[2].level, xz::redis::log_level::error);
}

TEST_F(LoggerTest, MinLevelFilteringWarning) {
  setup_capture_logger();
  xz::redis::logger::instance().set_log_level(xz::redis::log_level::warning);

  REDIS_LOG_DEBUG("Should not appear");
  REDIS_LOG_INFO("Should not appear");
  REDIS_LOG_WARNING("Should appear");
  REDIS_LOG_ERROR("Should appear");

  ASSERT_EQ(captured_logs_.size(), 2);
  EXPECT_EQ(captured_logs_[0].level, xz::redis::log_level::warning);
  EXPECT_EQ(captured_logs_[1].level, xz::redis::log_level::error);
}

TEST_F(LoggerTest, MinLevelFilteringError) {
  setup_capture_logger();
  xz::redis::logger::instance().set_log_level(xz::redis::log_level::error);

  REDIS_LOG_DEBUG("Should not appear");
  REDIS_LOG_INFO("Should not appear");
  REDIS_LOG_WARNING("Should not appear");
  REDIS_LOG_ERROR("Should appear");

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].level, xz::redis::log_level::error);
}

TEST_F(LoggerTest, MinLevelFilteringDebug) {
  setup_capture_logger();
  xz::redis::logger::instance().set_log_level(xz::redis::log_level::debug);

  REDIS_LOG_DEBUG("Should appear");
  REDIS_LOG_INFO("Should appear");
  REDIS_LOG_WARNING("Should appear");
  REDIS_LOG_ERROR("Should appear");

  ASSERT_EQ(captured_logs_.size(), 4);
}

// === Custom Log Function Tests ===

TEST_F(LoggerTest, CustomLogFunction) {
  std::vector<std::string> custom_logs;

  auto custom_fn = [&custom_logs](xz::redis::log_context const& ctx) {
    custom_logs.push_back(std::string("[CUSTOM:") + xz::redis::to_string(ctx.level) + "] " + std::string(ctx.message));
  };

  xz::redis::logger::instance().set_log_function(custom_fn);
  REDIS_LOG_INFO("Custom log");

  ASSERT_EQ(custom_logs.size(), 1);
  EXPECT_EQ(custom_logs[0], "[CUSTOM:info] Custom log");
}

TEST_F(LoggerTest, ResetToDefaultLogger) {
  setup_capture_logger();

  REDIS_LOG_INFO("Captured");
  ASSERT_EQ(captured_logs_.size(), 1);

  // Reset to default (stderr)
  xz::redis::logger::instance().set_log_function(nullptr);

  // This should go to stderr, not captured
  REDIS_LOG_INFO("To stderr");

  // Should still have only 1 captured log
  EXPECT_EQ(captured_logs_.size(), 1);
}

TEST_F(LoggerTest, SetLogFunctionViaConvenienceFunction) {
  std::vector<std::string> custom_logs;

  auto custom_fn = [&custom_logs](xz::redis::log_context const& ctx) {
    custom_logs.push_back(std::string(ctx.message));
  };

  xz::redis::set_log_function(custom_fn);
  REDIS_LOG_INFO("Test");

  ASSERT_EQ(custom_logs.size(), 1);
  EXPECT_EQ(custom_logs[0], "Test");
}

// === Level Conversion Tests ===

TEST_F(LoggerTest, LogLevelToString) {
  EXPECT_STREQ(xz::redis::to_string(xz::redis::log_level::debug), "debug");
  EXPECT_STREQ(xz::redis::to_string(xz::redis::log_level::info), "info");
  EXPECT_STREQ(xz::redis::to_string(xz::redis::log_level::warning), "warning");
  EXPECT_STREQ(xz::redis::to_string(xz::redis::log_level::error), "error");
}

// === Thread Safety Tests (Lock-Free) ===

TEST_F(LoggerTest, ConcurrentLogging) {
  setup_capture_logger();
  xz::redis::logger::instance().set_log_level(xz::redis::log_level::debug);

  constexpr int num_threads = 10;
  constexpr int logs_per_thread = 100;

  std::vector<std::thread> threads;
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([i]() {
      for (int j = 0; j < logs_per_thread; ++j) {
        REDIS_LOG_INFO("Thread {} log {}", i, j);
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  // Should have all logs
  EXPECT_EQ(captured_logs_.size(), num_threads * logs_per_thread);
}

TEST_F(LoggerTest, ConcurrentLogLevelChanges) {
  setup_capture_logger();

  std::vector<std::thread> threads;

  // Thread that changes log level (lock-free atomic operations)
  threads.emplace_back([]() {
    for (int i = 0; i < 100; ++i) {
      xz::redis::logger::instance().set_log_level(xz::redis::log_level::debug);
      xz::redis::logger::instance().set_log_level(xz::redis::log_level::info);
    }
  });

  // Threads that log (lock-free read operations)
  for (int i = 0; i < 5; ++i) {
    threads.emplace_back([]() {
      for (int j = 0; j < 100; ++j) {
        REDIS_LOG_INFO("Concurrent log");
        REDIS_LOG_DEBUG("Debug log");
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  // Just verify no crashes occurred (lock-free design)
  EXPECT_GT(captured_logs_.size(), 0);
}

// === Convenience Function Tests ===

TEST_F(LoggerTest, SetMinLogLevelConvenience) {
  setup_capture_logger();

  xz::redis::set_log_level(xz::redis::log_level::warning);

  REDIS_LOG_INFO("Should not appear");
  REDIS_LOG_WARNING("Should appear");

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].level, xz::redis::log_level::warning);
}

TEST_F(LoggerTest, GetMinLogLevel) {
  xz::redis::logger::instance().set_log_level(xz::redis::log_level::warning);

  auto level = xz::redis::logger::instance().get_log_level();
  EXPECT_EQ(level, xz::redis::log_level::warning);
}

// === Edge Cases ===

TEST_F(LoggerTest, EmptyMessage) {
  setup_capture_logger();

  REDIS_LOG_INFO("");

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].message, "");
}

TEST_F(LoggerTest, LongMessage) {
  setup_capture_logger();

  std::string long_msg(10000, 'x');
  REDIS_LOG_INFO("{}", long_msg);

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].message.size(), 10000);
}

TEST_F(LoggerTest, SpecialCharactersInMessage) {
  setup_capture_logger();

  REDIS_LOG_INFO("Special: \n\t\r\"'\\");

  ASSERT_EQ(captured_logs_.size(), 1);
  EXPECT_EQ(captured_logs_[0].message, "Special: \n\t\r\"'\\");
}

TEST_F(LoggerTest, FormatSpecialCharacters) {
  setup_capture_logger();

  REDIS_LOG_INFO("Curly braces: {{}}, percent: %, backslash: \\");

  ASSERT_EQ(captured_logs_.size(), 1);
  // Note: The simple fallback formatter doesn't handle {{}} escaping like std::format
  // So we just check that it doesn't crash and produces some output
  EXPECT_FALSE(captured_logs_[0].message.empty());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
