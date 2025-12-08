#pragma once

#include <xz/redis/resp3/parser.hpp>
#include <xz/redis/resp3/type.hpp>

#include <xz/redis/assert.hpp>

#include <string>
#include <tuple>

namespace xz::redis::resp3 {

void add_header(std::string& payload, type3 type, std::size_t size);
void add_blob(std::string& payload, std::string_view blob);
void add_separator(std::string& payload);

}  // namespace xz::redis::resp3
