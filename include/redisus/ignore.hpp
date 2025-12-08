#pragma once

#include <tuple>

namespace redisus {

using ignore_t = std::decay_t<decltype(std::ignore)>;

}  // namespace redisus
