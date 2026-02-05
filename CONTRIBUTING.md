# Contributing

Thanks for your interest in contributing to `rediscoro`.

## Development requirements

- Linux
- CMake >= 3.15
- C++20 compiler (GCC / Clang)
- GTest (for tests)

## Build & test

```bash
./build.sh -t
```

Sanitizers (Debug):

```bash
./build.sh -t --asan
./build.sh -t --ubsan
./build.sh -t --tsan
```

## Style

- Keep changes small and focused.
- Prefer clear contracts and comments for concurrency/cancellation behavior.
- Ensure all `if` statements use braces `{}`.

## Pull requests

- Include tests for bug fixes when practical.
- Keep public API changes well-motivated (this is a 0.x preview, but we still try to avoid churn).

