#pragma once

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

#if defined(__cpp_lib_format) && __has_include(<format>)
#include <format>
namespace rediscoro {
namespace format_impl = std;
#else
#ifndef FMT_HEADER_ONLY
#define FMT_HEADER_ONLY
#endif
#include <fmt/format.h>
namespace rediscoro {
namespace format_impl = fmt;
#endif

enum class log_level {
  debug,
  info,
  warning,
  error,
  off,
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
    case log_level::off:
      return "off";
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

using log_function = void(*)(void*, log_context const&);

class logger {
 public:
  static auto instance() -> logger& {
    static logger inst;
    return inst;
  }

  // IMPORTANT: Must be called before any logging operations to avoid race conditions
  void set_log_function(log_function fn, void* user_data = nullptr) {
    if (fn != nullptr) {
      log_fn_ = fn;
      log_user_data_ = user_data;
      return;
    }

    log_fn_ = &default_log_function;
    log_user_data_ = nullptr;
  }

  void set_log_level(log_level level) { min_level_.store(level, std::memory_order_relaxed); }

  auto get_log_level() const -> log_level { return min_level_.load(std::memory_order_relaxed); }

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
      log_fn_(log_user_data_, ctx);
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
  // Default: disable all logs unless user explicitly enables them.
  logger() : log_fn_(&default_log_function), log_user_data_(nullptr), min_level_(log_level::off) {}

  static void default_log_function(void*, log_context const& ctx) {
    auto time = std::chrono::system_clock::to_time_t(ctx.timestamp);
    std::tm tm{};
    localtime_r(&time, &tm);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      ctx.timestamp.time_since_epoch()) % 1000;

    // Extract filename from path
    auto file = [&]() -> std::string_view {
      auto path = ctx.file;

      // Prefer showing the path relative to the `rediscoro/` directory (excluding `rediscoro` itself).
      constexpr std::string_view k_rediscoro_posix = "rediscoro/";
      constexpr std::string_view k_rediscoro_win = "rediscoro\\";
      if (auto pos = path.find(k_rediscoro_posix); pos != std::string_view::npos) {
        return path.substr(pos + k_rediscoro_posix.size());
      }
      if (auto pos = path.find(k_rediscoro_win); pos != std::string_view::npos) {
        return path.substr(pos + k_rediscoro_win.size());
      }

      // Fallback: basename only.
      if (auto pos = path.find_last_of("/\\"); pos != std::string_view::npos) {
        return path.substr(pos + 1);
      }
      return path;
    }();

    auto formatted = format_impl::format(
      "[{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}] [rediscoro] [{}] [{}:{}] {}",
      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
      tm.tm_hour, tm.tm_min, tm.tm_sec, ms.count(),
      to_string(ctx.level), file, ctx.line, ctx.message
    );

    std::cerr << formatted << std::endl;
  }

  log_function log_fn_;
  void* log_user_data_;
  std::atomic<log_level> min_level_;
};

inline auto get_logger() -> logger& { return logger::instance(); }

// IMPORTANT: Must be called before any logging operations
inline void set_log_function(log_function fn, void* user_data = nullptr) { logger::instance().set_log_function(fn, user_data); }

inline void set_log_level(log_level level) { logger::instance().set_log_level(level); }

}  // namespace rediscoro

#define REDISCORO_LOG_DEBUG(fmt, ...) \
  ::rediscoro::get_logger().log(::rediscoro::log_level::debug, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)

#define REDISCORO_LOG_INFO(fmt, ...) \
  ::rediscoro::get_logger().log(::rediscoro::log_level::info, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)

#define REDISCORO_LOG_WARNING(fmt, ...) \
  ::rediscoro::get_logger().log(::rediscoro::log_level::warning, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)

#define REDISCORO_LOG_ERROR(fmt, ...) \
  ::rediscoro::get_logger().log(::rediscoro::log_level::error, __FILE__, __LINE__, fmt __VA_OPT__(, ) __VA_ARGS__)
