#pragma once

#include <redisus/adapter/result.hpp>

namespace redisus {

template <class... Ts>
using response = std::tuple<adapter::result<Ts>...>;

using generic_response = adapter::result<std::vector<resp3::node>>;

}  // namespace redisus
