#pragma once

#include <redisus/adapter/result.hpp>

namespace redisus {

template <class... Ts>
using response = std::tuple<adapter::result<Ts>...>;

}  // namespace redisus
