#pragma once

#include <tuple>

namespace xz::redis {

using ignore_t = std::decay_t<decltype(std::ignore)>;

}  // namespace xz::redis
