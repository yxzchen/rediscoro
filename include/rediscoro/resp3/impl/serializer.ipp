#include <rediscoro/resp3/parser.hpp>
#include <rediscoro/resp3/serializer.hpp>

namespace rediscoro::resp3 {

void add_header(std::string& payload, type3 type, std::size_t size) {
  auto const str = std::to_string(size);

  payload += to_code(type);
  payload.append(std::cbegin(str), std::cend(str));
  payload += parser::sep;
}

void add_blob(std::string& payload, std::string_view blob) {
  payload.append(std::cbegin(blob), std::cend(blob));
  payload += parser::sep;
}

void add_separator(std::string& payload) { payload += parser::sep; }

void to_bulk(std::string& payload, std::string_view data) {
  auto const str = std::to_string(data.size());

  payload += to_code(type3::blob_string);
  payload.append(std::cbegin(str), std::cend(str));
  payload += parser::sep;
  payload.append(std::cbegin(data), std::cend(data));
  payload += parser::sep;
}

}  // namespace rediscoro::resp3
