#pragma once

#include <cstdlib>
#include <exception>

#if defined(__GNUC__) || defined(__clang__)
#define REDISCORO_LIKELY(x) __builtin_expect(!!(x), 1)
#define REDISCORO_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define REDISCORO_LIKELY(x) (x)
#define REDISCORO_UNLIKELY(x) (x)
#endif

namespace rediscoro::detail {

[[noreturn]] void assert_fail(char const* expr, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void assert_fail(char const* expr, char const* msg, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void ensure_fail(char const* expr, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void ensure_fail(char const* expr, char const* msg, char const* file, int line,
                              char const* func) noexcept;

[[noreturn]] void unreachable_fail(char const* file, int line, char const* func) noexcept;

}  // namespace rediscoro::detail

// -------------------- ASSERT --------------------
#if !defined(NDEBUG)

#define REDISCORO_ASSERT_SELECTOR(_1, _2, NAME, ...) NAME

#define REDISCORO_ASSERT_1(expr)    \
  (REDISCORO_LIKELY(expr) ? (void)0 \
                          : ::rediscoro::detail::assert_fail(#expr, __FILE__, __LINE__, __func__))

#define REDISCORO_ASSERT_2(expr, msg) \
  (REDISCORO_LIKELY(expr)             \
     ? (void)0                        \
     : ::rediscoro::detail::assert_fail(#expr, msg, __FILE__, __LINE__, __func__))

#define REDISCORO_ASSERT(...) \
  REDISCORO_ASSERT_SELECTOR(__VA_ARGS__, REDISCORO_ASSERT_2, REDISCORO_ASSERT_1)(__VA_ARGS__)

#else
#define REDISCORO_ASSERT(...) ((void)0)
#endif

// -------------------- ENSURE --------------------

#define REDISCORO_ENSURE_SELECTOR(_1, _2, NAME, ...) NAME

#define REDISCORO_ENSURE_1(expr)    \
  (REDISCORO_LIKELY(expr) ? (void)0 \
                          : ::rediscoro::detail::ensure_fail(#expr, __FILE__, __LINE__, __func__))

#define REDISCORO_ENSURE_2(expr, msg) \
  (REDISCORO_LIKELY(expr)             \
     ? (void)0                        \
     : ::rediscoro::detail::ensure_fail(#expr, msg, __FILE__, __LINE__, __func__))

#define REDISCORO_ENSURE(...) \
  REDISCORO_ENSURE_SELECTOR(__VA_ARGS__, REDISCORO_ENSURE_2, REDISCORO_ENSURE_1)(__VA_ARGS__)

// -------------------- UNREACHABLE --------------------

#define REDISCORO_UNREACHABLE() ::rediscoro::detail::unreachable_fail(__FILE__, __LINE__, __func__)
