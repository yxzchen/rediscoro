#pragma once

#include <coroutine>
#include <exception>

namespace rediscoro::resp3 {

template <typename T>
class generator {
 public:
  struct promise_type {
    T current_value;
    std::exception_ptr exception_;

    generator get_return_object() { return generator{std::coroutine_handle<promise_type>::from_promise(*this)}; }

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    template <std::convertible_to<T> From>
    std::suspend_always yield_value(From&& from) {
      current_value = std::forward<From>(from);
      return {};
    }

    void return_void() noexcept {}
    void unhandled_exception() noexcept { exception_ = std::current_exception(); }
  };

  using handle_type = std::coroutine_handle<promise_type>;

  explicit generator(handle_type handle) : handle_(handle) {}
  ~generator() { handle_.destroy(); }

  generator(const generator&) = delete;
  generator& operator=(const generator&) = delete;
  generator(generator&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
  generator& operator=(generator&& other) noexcept {
    if (this != &other) {
      if (handle_) handle_.destroy();
      handle_ = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  bool next() {
    if (!handle_ || handle_.done()) return false;
    handle_.resume();
    if (handle_.promise().exception_) {
      std::rethrow_exception(handle_.promise().exception_);
    }
    return !handle_.done();
  }

  T& value() { return handle_.promise().current_value; }
  const T& value() const { return handle_.promise().current_value; }

 private:
  handle_type handle_;
};

}  // namespace rediscoro::resp3
