#include <redisus/resp3/parser.hpp>
#include <redisus/resp3/impl/parser.ipp>
#include <redisus/impl/error.ipp>
#include <iostream>
#include <cassert>
#include <string>

void test_partial_feeding() {
  redisus::resp3::parser p;
  std::error_code ec;

  auto gen = p.parse(ec);

  // Feed first chunk of Message 1: +OK\r\n (simple string)
  p.feed("+OK");
  assert(gen.next());
  auto result = gen.value();
  assert(!result);
  std::cout << "Message 1 (partial): needs more data\n";

  // Feed rest of Message 1
  p.feed("\r\n");
  assert(gen.next());
  result = gen.value();
  assert(result.has_value());
  assert(result->size() == 1);
  assert(result->at(0).data_type == redisus::resp3::type3::simple_string);
  std::cout << "Message 1: simple string parsed successfully\n";

  // Feed partial Message 2: *2\r\n+foo\r\n+bar\r\n (array with 2 strings)
  p.feed("*2\r\n+f");
  assert(gen.next());
  result = gen.value();
  assert(!result);  // Need more data
  std::cout << "Message 2 (partial): needs more data\n";

  // Feed more partial data
  p.feed("oo\r\n+ba");
  assert(gen.next());
  result = gen.value();
  assert(!result);  // Still need more data
  std::cout << "Message 2 (partial): still needs more data\n";

  // Feed rest of Message 2
  p.feed("r\r\n");
  assert(gen.next());
  result = gen.value();
  assert(result.has_value());
  assert(result->size() == 3);  // array header + 2 elements
  assert(result->at(0).data_type == redisus::resp3::type3::array);
  assert(result->at(1).data_type == redisus::resp3::type3::simple_string);
  assert(result->at(2).data_type == redisus::resp3::type3::simple_string);
  std::cout << "Message 2: array parsed successfully\n";

  // Feed partial Message 3: :42\r\n (number)
  p.feed(":");
  assert(gen.next());
  result = gen.value();
  assert(!result);  // Need more data
  std::cout << "Message 3 (partial): needs more data\n";

  // Feed rest of Message 3
  p.feed("42\r\n");
  assert(gen.next());
  result = gen.value();
  assert(result.has_value());
  assert(result->size() == 1);
  assert(result->at(0).data_type == redisus::resp3::type3::number);
  std::cout << "Message 3: number parsed successfully\n";

  std::cout << "\nAll partial feeding tests passed!\n";
}

int main() {
  try {
    test_partial_feeding();
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Test failed with exception: " << e.what() << "\n";
    return 1;
  }
}
