# Redisus Adapters

The adapter module provides a simple way to convert RESP3 protocol messages directly into C++ standard library types.

## Features

- **Simple type conversions**: string, int, double, bool
- **Container adapters**: `std::vector`, `std::map`, `std::optional`
- **Automatic type conversion**: Converts RESP3 nodes to appropriate C++ types
- **Error handling**: Uses `std::error_code` for error reporting

## Usage

### Quick Start

```cpp
#include <redisus/adapter/adapt.hpp>

std::string result;
std::error_code ec;
redisus::adapter::parse("+OK\r\n", result, ec);
// result == "OK"
```

### Supported Types

#### Simple Types

- `std::string` - RESP3 simple strings, bulk strings
- `std::string_view` - Non-owning string references
- `int`, `long`, etc. - RESP3 numbers
- `double` - RESP3 doubles
- `bool` - RESP3 booleans

#### Container Types

- `std::vector<T>` - RESP3 arrays, sets, push
- `std::map<K, V>` - RESP3 maps
- `std::optional<T>` - Handles null values

#### Special Adapters

- `adapter::ignore` - Discards all parsed values (useful for validation)

### Examples

#### Parse a vector of integers

```cpp
std::vector<int> numbers;
std::error_code ec;
adapter::parse("*3\r\n:1\r\n:2\r\n:3\r\n", numbers, ec);
// numbers == {1, 2, 3}
```

#### Parse a map

```cpp
std::map<std::string, int> data;
std::error_code ec;
adapter::parse("%2\r\n+age\r\n:30\r\n+score\r\n:100\r\n", data, ec);
// data == {{"age", 30}, {"score", 100}}
```

#### Handle optional values (null)

```cpp
std::optional<std::string> value;
std::error_code ec;

adapter::parse("_\r\n", value, ec);  // null
// value.has_value() == false

adapter::parse("+hello\r\n", value, ec);
// value == "hello"
```

#### Validate RESP3 messages (ignore adapter)

```cpp
std::error_code ec;
resp3::parser p;

// Parse and validate without storing results
auto success = resp3::parse(p, "*3\r\n+a\r\n+b\r\n+c\r\n", adapter::ignore, ec);
if (!ec) {
  // Message is valid RESP3
}
```

### Manual Adapter Usage

For more control, you can create adapters manually:

```cpp
std::vector<std::string> result;
std::error_code ec;

resp3::parser p;
auto adapter = adapter::make_adapter(result);
resp3::parse(p, "*2\r\n+one\r\n+two\r\n", adapter, ec);
```

## Limitations

The current adapter implementation is intentionally simple and does not support:

- Nested aggregates (arrays of arrays, maps of maps, etc.)
- Custom type conversions (use manual adapter implementation)
- Streaming or incremental parsing (use the low-level parser API)

For these advanced use cases, you can implement a custom adapter by providing a class with an `on_node(node_view const&, std::error_code&)` method.

## Error Handling

All adapter functions use `std::error_code` for error reporting. Common errors:

- `error::expects_resp3_simple_type` - Tried to parse aggregate as simple type
- `error::not_a_number` - String cannot be converted to number
- `error::nested_aggregate_not_supported` - Nested containers not supported
