#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>

#ifdef REDISXZ_USE_STDFMT
#include <format>
namespace xz::redis {
namespace format_impl = std;
#else
#include <fmt/format.h>
#include <fmt/chrono.h>
namespace xz::redis {
namespace format_impl = fmt;
#endif

enum class log_level {
  debug,
  info,
  warning,
  error,
};

constexpr auto to_string(log_level level) noexcept -> char const* {
  switch (level) {
    case log_level::debug:
      return "debug";
    case log_level::info:
      return "info";
    case log_level::warning:
      return "warning";
    case log_level::error:
      return "error";
    default:
      return "unknown";
  }
}

struct log_context {
  log_level level;
  std::string_view message;
  std::string_view file;
  int line;
  std::chrono::system_clock::time_point timestamp;
};

using log_function = std::function<void(log_context const&)>;

class logger {
 public:
  static auto instance() -> logger& {
    static logger inst;
    return inst;
  }

  // IMPORTANT: Must be called before any logging operations to avoid race conditions
  void set_log_function(log_function fn) {
    if (fn) {
      log_fn_ = std::move(fn);
    } else {
      log_fn_ = default_log_function();
    }
  }

  void set_min_level(log_level level) { min_level_.store(level, std::memory_order_relaxed); }

  auto get_min_level() const -> log_level { return min_level_.load(std::memory_order_relaxed); }

  void log(log_level level, std::string_view message, std::string_view file, int line) {
    if (level < min_level_.load(std::memory_order_relaxed)) {
      return;
    }

    if (log_fn_) {
      log_context ctx{
        .level = level,
        .message = message,
        .file = file,
        .line = line,
        .timestamp = std::chrono::system_clock::now()
      };
      log_fn_(ctx);
    }
  }

  template <typename... Args>
  void log(log_level level, std::string_view file, int line, format_impl::format_string<Args...> fmt, Args&&... args) {
    if (level < min_level_.load(std::memory_order_relaxed)) {
      return;
    }

    auto message = format_impl::format(fmt, std::forward<Args>(args)...);
    log(level, message, file, line);
  }

 private:
  logger() : log_fn_(default_log_function()), min_level_(log_level::info) {}

  static auto default_log_function() -> log_function {
    return [](log_context const& ctx) {
      auto time = std::chrono::system_clock::to_time_t(ctx.timestamp);
      auto tm = *std::localtime(&time);
      auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        ctx.timestamp.time_since_epoch()) % 1000;

      // Extract filename from path
      auto file = ctx.file;
      auto pos = file.find_last_of("/\\");
      if (pos != std::string_view::npos) {
        file = file.substr(pos + 1);
      }

      auto formatted = format_impl::format(
        "[{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}] [redisxz] [{}] [{}:{}] {}",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, ms.count(),
        to_string(ctx.level), file, ctx.line, ctx.message
      );

      std::cerr << formatted << std::endl;
    };
  }

  log_function log_fn_;
  std::atomic<log_level> min_level_;
};

inline auto get_logger() -> logger& { return logger::instance(); }

// IMPORTANT: Must be called before any logging operations
inline void set_log_function(log_function fn) { logger::instance().set_log_function(std::move(fn)); }

inline void set_min_log_level(log_level level) { logger::instance().set_min_level(level); }

}  // namespace xz::redis

#define REDIS_LOG_DEBUG(fmt, ...) \
  ::xz::redis::get_logger().log(::xz::redis::log_level::debug, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)

#define REDIS_LOG_INFO(fmt, ...) \
  ::xz::redis::get_logger().log(::xz::redis::log_level::info, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)

#define REDIS_LOG_WARNING(fmt, ...) \
  ::xz::redis::get_logger().log(::xz::redis::log_level::warning, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)

#define REDIS_LOG_ERROR(fmt, ...) \
  ::xz::redis::get_logger().log(::xz::redis::log_level::error, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)
