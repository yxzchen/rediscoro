#pragma once

#include <rediscoro/assert.hpp>
#include <rediscoro/resp3/message.hpp>
#include <rediscoro/resp3/visitor.hpp>

#include <charconv>
#include <cmath>
#include <cstdint>
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
    encode_with_attrs(msg);
    return std::move(buffer_);
  }

  /// Encode to an existing string buffer
  auto encode_to(std::string& out, const message& msg) -> void {
    buffer_.clear();
    encode_with_attrs(msg);
    out.append(buffer_);
  }

  // Visitor interface
  auto operator()(const simple_string& val) -> void {
    append_prefix(kind::simple_string);
    buffer_ += val.data;
    append_crlf();
  }

  auto operator()(const simple_error& val) -> void {
    append_prefix(kind::simple_error);
    buffer_ += val.message;
    append_crlf();
  }

  auto operator()(const integer& val) -> void {
    append_prefix(kind::integer);
    append_i64(val.value);
    append_crlf();
  }

  auto operator()(const double_number& val) -> void { append_double(val.value); }

  auto operator()(const boolean& val) -> void {
    append_prefix(kind::boolean);
    buffer_ += val.value ? 't' : 'f';
    append_crlf();
  }

  auto operator()(const big_number& val) -> void {
    append_prefix(kind::big_number);
    buffer_ += val.value;
    append_crlf();
  }

  auto operator()(const null&) -> void {
    append_prefix(kind::null);
    append_crlf();
  }

  auto operator()(const bulk_string& val) -> void {
    append_prefix(kind::bulk_string);
    append_size(val.data.size());
    append_crlf();
    buffer_ += val.data;
    append_crlf();
  }

  auto operator()(const bulk_error& val) -> void {
    append_prefix(kind::bulk_error);
    append_size(val.message.size());
    append_crlf();
    buffer_ += val.message;
    append_crlf();
  }

  auto operator()(const verbatim_string& val) -> void {
    // Format: =<length>\r\n<encoding>:<data>\r\n
    // encoding is 3 bytes
    auto total_len = val.encoding.size() + 1 + val.data.size();  // encoding + ':' + data
    append_prefix(kind::verbatim_string);
    append_size(total_len);
    append_crlf();
    buffer_ += val.encoding;
    buffer_ += ':';
    buffer_ += val.data;
    append_crlf();
  }

  auto operator()(const array& val) -> void {
    append_prefix(kind::array);
    append_size(val.elements.size());
    append_crlf();

    for (const auto& elem : val.elements) {
      encode_message(elem);
    }
  }

  auto operator()(const map& val) -> void {
    append_prefix(kind::map);
    append_size(val.entries.size());
    append_crlf();

    for (const auto& [key, value] : val.entries) {
      encode_message(key);
      encode_message(value);
    }
  }

  auto operator()(const set& val) -> void {
    append_prefix(kind::set);
    append_size(val.elements.size());
    append_crlf();

    for (const auto& elem : val.elements) {
      encode_message(elem);
    }
  }

  auto operator()(const push& val) -> void {
    append_prefix(kind::push);
    append_size(val.elements.size());
    append_crlf();

    for (const auto& elem : val.elements) {
      encode_message(elem);
    }
  }

 private:
  std::string buffer_;

  auto append_prefix(kind k) -> void { buffer_.push_back(kind_to_prefix(k)); }

  auto append_crlf() -> void { buffer_.append("\r\n"); }

  auto append_u64(std::uint64_t v) -> void {
    char tmp[32]{};
    auto res = std::to_chars(tmp, tmp + sizeof(tmp), v);
    REDISCORO_ASSERT(res.ec == std::errc{});
    buffer_.append(tmp, res.ptr);
  }

  auto append_size(std::size_t v) -> void { append_u64(static_cast<std::uint64_t>(v)); }

  auto append_i64(std::int64_t v) -> void {
    char tmp[32]{};
    auto res = std::to_chars(tmp, tmp + sizeof(tmp), v);
    REDISCORO_ASSERT(res.ec == std::errc{});
    buffer_.append(tmp, res.ptr);
  }

  auto append_double(double v) -> void {
    append_prefix(kind::double_number);

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
      auto res = std::to_chars(tmp, tmp + sizeof(tmp), v, std::chars_format::general,
                               std::numeric_limits<double>::max_digits10);
      if (res.ec != std::errc{}) {
        buffer_ += "nan";
      } else {
        buffer_.append(tmp, res.ptr);
      }
    }

    append_crlf();
  }

  auto encode_message(const message& msg) -> void { encode_with_attrs(msg); }

  auto encode_with_attrs(const message& msg) -> void {
    if (msg.has_attributes()) {
      encode_attribute(msg.get_attributes());
    }
    visit(*this, msg);
  }

  auto encode_attribute(const attribute& attr) -> void {
    append_prefix(kind::attribute);
    append_size(attr.entries.size());
    append_crlf();

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
