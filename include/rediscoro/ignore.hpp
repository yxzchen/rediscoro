#pragma once

#include <tuple>

namespace rediscoro {

using ignore_t = std::decay_t<decltype(std::ignore)>;

}  // namespace rediscoro
