#pragma once

#include <xz/redis/adapter/result.hpp>

namespace xz::redis {

template <class... Ts>
using response = std::tuple<adapter::result<Ts>...>;

using generic_response = adapter::result<std::vector<resp3::node>>;

}  // namespace xz::redis
