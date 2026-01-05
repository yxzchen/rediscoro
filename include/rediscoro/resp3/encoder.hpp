#pragma once

#include <rediscoro/resp3/message.hpp>
#include <rediscoro/resp3/visitor.hpp>

#include <charconv>
#include <cmath>
#include <limits>
#include <string>

namespace rediscoro::resp3 {

/// RESP3 encoder that serializes messages to byte stream
class encoder {
public:
  encoder() = default;

  /// Encode a message to RESP3 protocol format
  auto encode(const message& msg) -> std::string {
    buffer_.clear();

    // Encode attributes first if present
    if (msg.has_attributes()) {
      encode_attribute(msg.get_attributes());
    }

    // Encode the value
    visit(*this, msg);

    return std::move(buffer_);
  }

  /// Encode to an existing string buffer
  auto encode_to(std::string& out, const message& msg) -> void {
    buffer_.clear();

    if (msg.has_attributes()) {
      encode_attribute(msg.get_attributes());
    }

    visit(*this, msg);
    out.append(buffer_);
  }

  // Visitor interface
  auto operator()(const simple_string& val) -> void {
    buffer_ += '+';
    buffer_ += val.data;
    buffer_ += "\r\n";
  }

  auto operator()(const simple_error& val) -> void {
    buffer_ += '-';
    buffer_ += val.message;
    buffer_ += "\r\n";
  }

  auto operator()(const integer& val) -> void {
    buffer_ += ':';
    buffer_ += std::to_string(val.value);
    buffer_ += "\r\n";
  }

  auto operator()(const double_type& val) -> void {
    append_double(val.value);
  }

  auto operator()(const boolean& val) -> void {
    buffer_ += '#';
    buffer_ += val.value ? 't' : 'f';
    buffer_ += "\r\n";
  }

  auto operator()(const big_number& val) -> void {
    buffer_ += '(';
    buffer_ += val.value;
    buffer_ += "\r\n";
  }

  auto operator()(const null&) -> void {
    buffer_ += "_\r\n";
  }

  auto operator()(const bulk_string& val) -> void {
    buffer_ += '$';
    buffer_ += std::to_string(val.data.size());
    buffer_ += "\r\n";
    buffer_ += val.data;
    buffer_ += "\r\n";
  }

  auto operator()(const bulk_error& val) -> void {
    buffer_ += '!';
    buffer_ += std::to_string(val.message.size());
    buffer_ += "\r\n";
    buffer_ += val.message;
    buffer_ += "\r\n";
  }

  auto operator()(const verbatim_string& val) -> void {
    // Format: =<length>\r\n<encoding>:<data>\r\n
    // encoding is 3 bytes
    auto total_len = val.encoding.size() + 1 + val.data.size(); // encoding + ':' + data
    buffer_ += '=';
    buffer_ += std::to_string(total_len);
    buffer_ += "\r\n";
    buffer_ += val.encoding;
    buffer_ += ':';
    buffer_ += val.data;
    buffer_ += "\r\n";
  }

  auto operator()(const array& val) -> void {
    buffer_ += '*';
    buffer_ += std::to_string(val.elements.size());
    buffer_ += "\r\n";

    for (const auto& elem : val.elements) {
      encode_message(elem);
    }
  }

  auto operator()(const map& val) -> void {
    buffer_ += '%';
    buffer_ += std::to_string(val.entries.size());
    buffer_ += "\r\n";

    for (const auto& [key, value] : val.entries) {
      encode_message(key);
      encode_message(value);
    }
  }

  auto operator()(const set& val) -> void {
    buffer_ += '~';
    buffer_ += std::to_string(val.elements.size());
    buffer_ += "\r\n";

    for (const auto& elem : val.elements) {
      encode_message(elem);
    }
  }

  auto operator()(const push& val) -> void {
    buffer_ += '>';
    buffer_ += std::to_string(val.elements.size());
    buffer_ += "\r\n";

    for (const auto& elem : val.elements) {
      encode_message(elem);
    }
  }

private:
  std::string buffer_;

  auto append_double(double v) -> void {
    buffer_ += ',';

    if (std::isnan(v)) {
      buffer_ += "nan";
    } else if (std::isinf(v)) {
      if (v > 0) {
        buffer_ += "inf";
      } else {
        buffer_ += "-inf";
      }
    } else {
      char tmp[64]{};
      auto res = std::to_chars(
        tmp,
        tmp + sizeof(tmp),
        v,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10
      );
      if (res.ec != std::errc{}) {
        buffer_ += "nan";
      } else {
        buffer_.append(tmp, res.ptr);
      }
    }

    buffer_ += "\r\n";
  }

  auto encode_message(const message& msg) -> void {
    // Encode attributes first if present
    if (msg.has_attributes()) {
      encode_attribute(msg.get_attributes());
    }

    // Encode the value
    visit(*this, msg);
  }

  auto encode_attribute(const attribute& attr) -> void {
    buffer_ += '|';
    buffer_ += std::to_string(attr.entries.size());
    buffer_ += "\r\n";

    for (const auto& [key, value] : attr.entries) {
      encode_message(key);
      encode_message(value);
    }
  }
};

/// Convenience function to encode a message
inline auto encode(const message& msg) -> std::string {
  encoder enc;
  return enc.encode(msg);
}

}  // namespace rediscoro::resp3
