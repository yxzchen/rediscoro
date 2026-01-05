#pragma once

#include <charconv>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rediscoro::resp3::detail {

namespace {

constexpr std::size_t max_nesting_depth = 64;

[[nodiscard]] auto find_crlf(std::string_view sv) -> std::size_t {
  return sv.find("\r\n");
}

[[nodiscard]] auto parse_i64(std::string_view sv, std::int64_t& out) -> bool {
  if (sv.empty()) {
    return false;
  }
  auto first = sv.data();
  auto last = sv.data() + sv.size();
  auto res = std::from_chars(first, last, out);
  if (res.ec != std::errc{}) {
    return false;
  }
  if (res.ptr != last) {
    return false;
  }
  return true;
}

[[nodiscard]] auto parse_len(std::string_view sv, std::int64_t& out) -> bool {
  return parse_i64(sv, out);
}

[[nodiscard]] auto parse_double(std::string_view sv, double& out) -> bool {
  if (sv == "inf") {
    out = std::numeric_limits<double>::infinity();
    return true;
  }
  if (sv == "-inf") {
    out = -std::numeric_limits<double>::infinity();
    return true;
  }
  if (sv == "nan") {
    out = std::numeric_limits<double>::quiet_NaN();
    return true;
  }

  // strtod requires a null-terminated string.
  std::string tmp(sv);
  char* end = nullptr;
  out = std::strtod(tmp.c_str(), &end);
  if (end == tmp.c_str()) {
    return false;
  }
  if (end != tmp.c_str() + tmp.size()) {
    return false;
  }
  return true;
}

}  // namespace

class message_parser;

class simple_string_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return false;
    }
    if (data.empty() || data[0] != '+') {
      ec = make_error_code(error::invalid_format);
      return true;
    }
    out = message(simple_string{std::string(data.substr(1, pos - 1))});
    buf.consume(pos + 2);
    return true;
  }
};

class simple_error_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return false;
    }
    if (data.empty() || data[0] != '-') {
      ec = make_error_code(error::invalid_format);
      return true;
    }
    out = message(simple_error{std::string(data.substr(1, pos - 1))});
    buf.consume(pos + 2);
    return true;
  }
};

class integer_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return false;
    }
    if (data.empty() || data[0] != ':') {
      ec = make_error_code(error::invalid_format);
      return true;
    }
    std::int64_t v{};
    if (!parse_i64(data.substr(1, pos - 1), v)) {
      ec = make_error_code(error::invalid_integer);
      return true;
    }
    out = message(integer{v});
    buf.consume(pos + 2);
    return true;
  }
};

class double_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return false;
    }
    if (data.empty() || data[0] != ',') {
      ec = make_error_code(error::invalid_format);
      return true;
    }
    double v{};
    if (!parse_double(data.substr(1, pos - 1), v)) {
      ec = make_error_code(error::invalid_format);
      return true;
    }
    out = message(double_type{v});
    buf.consume(pos + 2);
    return true;
  }
};

class boolean_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override {
    auto data = buf.data();
    if (data.size() < 4) {
      return false;
    }
    if (data[0] != '#') {
      ec = make_error_code(error::invalid_format);
      return true;
    }
    if (data[3] != '\n' || data[2] != '\r') {
      ec = make_error_code(error::invalid_format);
      return true;
    }
    if (data[1] == 't') {
      out = message(boolean{true});
    } else if (data[1] == 'f') {
      out = message(boolean{false});
    } else {
      ec = make_error_code(error::invalid_format);
      return true;
    }
    buf.consume(4);
    return true;
  }
};

class big_number_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return false;
    }
    if (data.empty() || data[0] != '(') {
      ec = make_error_code(error::invalid_format);
      return true;
    }
    out = message(big_number{std::string(data.substr(1, pos - 1))});
    buf.consume(pos + 2);
    return true;
  }
};

class null_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override {
    auto data = buf.data();
    if (data.size() < 3) {
      return false;
    }
    if (data[0] != '_') {
      ec = make_error_code(error::invalid_format);
      return true;
    }
    if (data[1] != '\r' || data[2] != '\n') {
      ec = make_error_code(error::invalid_format);
      return true;
    }
    out = message(null{});
    buf.consume(3);
    return true;
  }
};

class bulk_string_parser final : public value_parser {
  enum class stage {
    read_len,
    read_data,
  };

  stage stage_{stage::read_len};
  std::int64_t expected_{0};

public:
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override {
    while (true) {
      if (stage_ == stage::read_len) {
        auto data = buf.data();
        auto pos = find_crlf(data);
        if (pos == std::string_view::npos) {
          return false;
        }
        if (data.empty() || data[0] != '$') {
          ec = make_error_code(error::invalid_format);
          return true;
        }
        std::int64_t len{};
        if (!parse_len(data.substr(1, pos - 1), len)) {
          ec = make_error_code(error::invalid_length);
          return true;
        }
        if (len < -1) {
          ec = make_error_code(error::invalid_length);
          return true;
        }
        buf.consume(pos + 2);
        if (len == -1) {
          out = message(null{});
          return true;
        }
        expected_ = len;
        stage_ = stage::read_data;
      }

      if (stage_ == stage::read_data) {
        auto data = buf.data();
        auto need = static_cast<std::size_t>(expected_) + 2;
        if (data.size() < need) {
          return false;
        }
        if (data.substr(static_cast<std::size_t>(expected_), 2) != "\r\n") {
          ec = make_error_code(error::invalid_format);
          return true;
        }
        out = message(bulk_string{std::string(data.substr(0, static_cast<std::size_t>(expected_)))});
        buf.consume(need);
        return true;
      }
    }
  }
};

class bulk_error_parser final : public value_parser {
  enum class stage {
    read_len,
    read_data,
  };

  stage stage_{stage::read_len};
  std::int64_t expected_{0};

public:
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override {
    while (true) {
      if (stage_ == stage::read_len) {
        auto data = buf.data();
        auto pos = find_crlf(data);
        if (pos == std::string_view::npos) {
          return false;
        }
        if (data.empty() || data[0] != '!') {
          ec = make_error_code(error::invalid_format);
          return true;
        }
        std::int64_t len{};
        if (!parse_len(data.substr(1, pos - 1), len)) {
          ec = make_error_code(error::invalid_length);
          return true;
        }
        if (len < 0) {
          ec = make_error_code(error::invalid_length);
          return true;
        }
        buf.consume(pos + 2);
        expected_ = len;
        stage_ = stage::read_data;
      }

      if (stage_ == stage::read_data) {
        auto data = buf.data();
        auto need = static_cast<std::size_t>(expected_) + 2;
        if (data.size() < need) {
          return false;
        }
        if (data.substr(static_cast<std::size_t>(expected_), 2) != "\r\n") {
          ec = make_error_code(error::invalid_format);
          return true;
        }
        out = message(bulk_error{std::string(data.substr(0, static_cast<std::size_t>(expected_)))});
        buf.consume(need);
        return true;
      }
    }
  }
};

class verbatim_string_parser final : public value_parser {
  enum class stage {
    read_len,
    read_data,
  };

  stage stage_{stage::read_len};
  std::int64_t expected_{0};

public:
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override {
    while (true) {
      if (stage_ == stage::read_len) {
        auto data = buf.data();
        auto pos = find_crlf(data);
        if (pos == std::string_view::npos) {
          return false;
        }
        if (data.empty() || data[0] != '=') {
          ec = make_error_code(error::invalid_format);
          return true;
        }
        std::int64_t len{};
        if (!parse_len(data.substr(1, pos - 1), len)) {
          ec = make_error_code(error::invalid_length);
          return true;
        }
        if (len < -1) {
          ec = make_error_code(error::invalid_length);
          return true;
        }
        buf.consume(pos + 2);
        if (len == -1) {
          out = message(null{});
          return true;
        }
        expected_ = len;
        stage_ = stage::read_data;
      }

      if (stage_ == stage::read_data) {
        auto data = buf.data();
        auto need = static_cast<std::size_t>(expected_) + 2;
        if (data.size() < need) {
          return false;
        }
        if (data.substr(static_cast<std::size_t>(expected_), 2) != "\r\n") {
          ec = make_error_code(error::invalid_format);
          return true;
        }
        auto payload = data.substr(0, static_cast<std::size_t>(expected_));
        if (payload.size() < 4) {
          ec = make_error_code(error::invalid_format);
          return true;
        }
        if (payload[3] != ':') {
          ec = make_error_code(error::invalid_format);
          return true;
        }
        verbatim_string v{};
        v.encoding = std::string(payload.substr(0, 3));
        v.data = std::string(payload.substr(4));
        out = message(std::move(v));
        buf.consume(need);
        return true;
      }
    }
  }
};

class array_parser final : public value_parser {
  enum class stage {
    read_len,
    read_elements,
  };

  stage stage_{stage::read_len};
  std::int64_t expected_{0};
  std::vector<message> elements_{};
  std::unique_ptr<value_parser> child_{};
  std::size_t depth_{0};

public:
  explicit array_parser(std::size_t depth) : depth_(depth) {}

  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override;
};

class set_parser final : public value_parser {
  enum class stage {
    read_len,
    read_elements,
  };

  stage stage_{stage::read_len};
  std::int64_t expected_{0};
  std::vector<message> elements_{};
  std::unique_ptr<value_parser> child_{};
  std::size_t depth_{0};

public:
  explicit set_parser(std::size_t depth) : depth_(depth) {}
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override;
};

class push_parser final : public value_parser {
  enum class stage {
    read_len,
    read_elements,
  };

  stage stage_{stage::read_len};
  std::int64_t expected_{0};
  std::vector<message> elements_{};
  std::unique_ptr<value_parser> child_{};
  std::size_t depth_{0};

public:
  explicit push_parser(std::size_t depth) : depth_(depth) {}
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override;
};

class map_parser final : public value_parser {
  enum class stage {
    read_len,
    read_key,
    read_value,
  };

  stage stage_{stage::read_len};
  std::int64_t expected_{0};
  std::vector<std::pair<message, message>> entries_{};
  std::unique_ptr<value_parser> child_{};
  std::optional<message> current_key_{};
  std::size_t depth_{0};

public:
  explicit map_parser(std::size_t depth) : depth_(depth) {}
  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override;
};

class attribute_value_parser final {
  enum class stage {
    read_len,
    read_key,
    read_value,
  };

  stage stage_{stage::read_len};
  std::int64_t expected_{0};
  std::vector<std::pair<message, message>> entries_{};
  std::unique_ptr<value_parser> child_{};
  std::optional<message> current_key_{};
  std::size_t depth_{0};

public:
  explicit attribute_value_parser(std::size_t depth) : depth_(depth) {}
  auto parse(buffer& buf, attribute& out, std::error_code& ec) -> bool;
};

[[nodiscard]] auto make_message_parser(std::size_t depth) -> std::unique_ptr<value_parser>;

[[nodiscard]] auto make_value_parser_for_type(char type_byte, std::size_t depth, std::error_code& ec)
  -> std::unique_ptr<value_parser> {
  if (depth > max_nesting_depth) {
    ec = make_error_code(error::nesting_too_deep);
    return nullptr;
  }

  switch (type_byte) {
    case '+': return std::make_unique<simple_string_parser>();
    case '-': return std::make_unique<simple_error_parser>();
    case ':': return std::make_unique<integer_parser>();
    case ',': return std::make_unique<double_parser>();
    case '#': return std::make_unique<boolean_parser>();
    case '(': return std::make_unique<big_number_parser>();
    case '_': return std::make_unique<null_parser>();
    case '$': return std::make_unique<bulk_string_parser>();
    case '!': return std::make_unique<bulk_error_parser>();
    case '=': return std::make_unique<verbatim_string_parser>();
    case '*': return std::make_unique<array_parser>(depth);
    case '%': return std::make_unique<map_parser>(depth);
    case '~': return std::make_unique<set_parser>(depth);
    case '>': return std::make_unique<push_parser>(depth);
    default:
      ec = make_error_code(error::invalid_type_byte);
      return nullptr;
  }
}

class message_parser final : public value_parser {
  enum class stage {
    read_attrs,
    read_value,
  };

  stage stage_{stage::read_attrs};
  std::optional<attribute> attrs_{};
  std::unique_ptr<value_parser> child_{};
  std::unique_ptr<attribute_value_parser> attr_child_{};
  std::size_t depth_{0};

public:
  explicit message_parser(std::size_t depth) : depth_(depth) {}

  auto parse(buffer& buf, message& out, std::error_code& ec) -> bool override {
    while (true) {
      auto data = buf.data();
      if (data.empty()) {
        return false;
      }

      if (stage_ == stage::read_attrs) {
        if (data[0] == '|') {
          if (!attr_child_) {
            attr_child_ = std::make_unique<attribute_value_parser>(depth_);
          }
          attribute tmp_attr{};
          std::error_code inner_ec{};
          auto done = attr_child_->parse(buf, tmp_attr, inner_ec);
          if (!done) {
            return false;
          }
          if (inner_ec) {
            ec = inner_ec;
            return true;
          }
          if (!attrs_.has_value()) {
            attrs_ = attribute{};
          }
          auto& dst = attrs_->entries;
          auto& src = tmp_attr.entries;
          dst.insert(dst.end(),
                     std::make_move_iterator(src.begin()),
                     std::make_move_iterator(src.end()));
          attr_child_.reset();
          continue;
        }
        stage_ = stage::read_value;
        continue;
      }

      if (stage_ == stage::read_value) {
        if (!child_) {
          std::error_code make_ec{};
          child_ = make_value_parser_for_type(data[0], depth_, make_ec);
          if (make_ec) {
            ec = make_ec;
            return true;
          }
          if (!child_) {
            ec = make_error_code(error::invalid_format);
            return true;
          }
        }

        message value_msg{};
        std::error_code inner_ec{};
        auto done = child_->parse(buf, value_msg, inner_ec);
        if (!done) {
          return false;
        }
        if (inner_ec) {
          ec = inner_ec;
          return true;
        }
        if (attrs_.has_value()) {
          value_msg.attrs = std::move(attrs_);
        }
        out = std::move(value_msg);
        return true;
      }
    }
  }
};

[[nodiscard]] auto make_message_parser(std::size_t depth) -> std::unique_ptr<value_parser> {
  if (depth > max_nesting_depth) {
    return nullptr;
  }
  return std::make_unique<message_parser>(depth);
}

auto array_parser::parse(buffer& buf, message& out, std::error_code& ec) -> bool {
  while (true) {
    if (stage_ == stage::read_len) {
      auto data = buf.data();
      auto pos = find_crlf(data);
      if (pos == std::string_view::npos) {
        return false;
      }
      if (data.empty() || data[0] != '*') {
        ec = make_error_code(error::invalid_format);
        return true;
      }
      std::int64_t len{};
      if (!parse_len(data.substr(1, pos - 1), len)) {
        ec = make_error_code(error::invalid_length);
        return true;
      }
      if (len < -1) {
        ec = make_error_code(error::invalid_length);
        return true;
      }
      buf.consume(pos + 2);
      if (len == -1) {
        out = message(null{});
        return true;
      }
      expected_ = len;
      elements_.clear();
      elements_.reserve(static_cast<std::size_t>(expected_));
      stage_ = stage::read_elements;
      if (expected_ == 0) {
        out = message(array{std::move(elements_)});
        return true;
      }
    }

    if (stage_ == stage::read_elements) {
      while (elements_.size() < static_cast<std::size_t>(expected_)) {
        if (!child_) {
          child_ = make_message_parser(depth_ + 1);
          if (!child_) {
            ec = make_error_code(error::nesting_too_deep);
            return true;
          }
        }
        message elem{};
        std::error_code inner_ec{};
        auto done = child_->parse(buf, elem, inner_ec);
        if (!done) {
          return false;
        }
        if (inner_ec) {
          ec = inner_ec;
          return true;
        }
        elements_.push_back(std::move(elem));
        child_.reset();
      }
      out = message(array{std::move(elements_)});
      return true;
    }
  }
}

auto set_parser::parse(buffer& buf, message& out, std::error_code& ec) -> bool {
  while (true) {
    if (stage_ == stage::read_len) {
      auto data = buf.data();
      auto pos = find_crlf(data);
      if (pos == std::string_view::npos) {
        return false;
      }
      if (data.empty() || data[0] != '~') {
        ec = make_error_code(error::invalid_format);
        return true;
      }
      std::int64_t len{};
      if (!parse_len(data.substr(1, pos - 1), len)) {
        ec = make_error_code(error::invalid_length);
        return true;
      }
      if (len < -1) {
        ec = make_error_code(error::invalid_length);
        return true;
      }
      buf.consume(pos + 2);
      if (len == -1) {
        out = message(null{});
        return true;
      }
      expected_ = len;
      elements_.clear();
      elements_.reserve(static_cast<std::size_t>(expected_));
      stage_ = stage::read_elements;
      if (expected_ == 0) {
        out = message(set{std::move(elements_)});
        return true;
      }
    }

    if (stage_ == stage::read_elements) {
      while (elements_.size() < static_cast<std::size_t>(expected_)) {
        if (!child_) {
          child_ = make_message_parser(depth_ + 1);
          if (!child_) {
            ec = make_error_code(error::nesting_too_deep);
            return true;
          }
        }
        message elem{};
        std::error_code inner_ec{};
        auto done = child_->parse(buf, elem, inner_ec);
        if (!done) {
          return false;
        }
        if (inner_ec) {
          ec = inner_ec;
          return true;
        }
        elements_.push_back(std::move(elem));
        child_.reset();
      }
      out = message(set{std::move(elements_)});
      return true;
    }
  }
}

auto push_parser::parse(buffer& buf, message& out, std::error_code& ec) -> bool {
  while (true) {
    if (stage_ == stage::read_len) {
      auto data = buf.data();
      auto pos = find_crlf(data);
      if (pos == std::string_view::npos) {
        return false;
      }
      if (data.empty() || data[0] != '>') {
        ec = make_error_code(error::invalid_format);
        return true;
      }
      std::int64_t len{};
      if (!parse_len(data.substr(1, pos - 1), len)) {
        ec = make_error_code(error::invalid_length);
        return true;
      }
      if (len < -1) {
        ec = make_error_code(error::invalid_length);
        return true;
      }
      buf.consume(pos + 2);
      if (len == -1) {
        out = message(null{});
        return true;
      }
      expected_ = len;
      elements_.clear();
      elements_.reserve(static_cast<std::size_t>(expected_));
      stage_ = stage::read_elements;
      if (expected_ == 0) {
        out = message(push{std::move(elements_)});
        return true;
      }
    }

    if (stage_ == stage::read_elements) {
      while (elements_.size() < static_cast<std::size_t>(expected_)) {
        if (!child_) {
          child_ = make_message_parser(depth_ + 1);
          if (!child_) {
            ec = make_error_code(error::nesting_too_deep);
            return true;
          }
        }
        message elem{};
        std::error_code inner_ec{};
        auto done = child_->parse(buf, elem, inner_ec);
        if (!done) {
          return false;
        }
        if (inner_ec) {
          ec = inner_ec;
          return true;
        }
        elements_.push_back(std::move(elem));
        child_.reset();
      }
      out = message(push{std::move(elements_)});
      return true;
    }
  }
}

auto map_parser::parse(buffer& buf, message& out, std::error_code& ec) -> bool {
  while (true) {
    if (stage_ == stage::read_len) {
      auto data = buf.data();
      auto pos = find_crlf(data);
      if (pos == std::string_view::npos) {
        return false;
      }
      if (data.empty() || data[0] != '%') {
        ec = make_error_code(error::invalid_format);
        return true;
      }
      std::int64_t len{};
      if (!parse_len(data.substr(1, pos - 1), len)) {
        ec = make_error_code(error::invalid_length);
        return true;
      }
      if (len < -1) {
        ec = make_error_code(error::invalid_length);
        return true;
      }
      buf.consume(pos + 2);
      if (len == -1) {
        out = message(null{});
        return true;
      }
      expected_ = len;
      entries_.clear();
      entries_.reserve(static_cast<std::size_t>(expected_));
      current_key_.reset();
      stage_ = stage::read_key;
      if (expected_ == 0) {
        out = message(map{std::move(entries_)});
        return true;
      }
    }

    if (stage_ == stage::read_key) {
      if (entries_.size() >= static_cast<std::size_t>(expected_)) {
        out = message(map{std::move(entries_)});
        return true;
      }
      if (!child_) {
        child_ = make_message_parser(depth_ + 1);
        if (!child_) {
          ec = make_error_code(error::nesting_too_deep);
          return true;
        }
      }
      message key{};
      std::error_code inner_ec{};
      auto done = child_->parse(buf, key, inner_ec);
      if (!done) {
        return false;
      }
      if (inner_ec) {
        ec = inner_ec;
        return true;
      }
      current_key_ = std::move(key);
      child_.reset();
      stage_ = stage::read_value;
      continue;
    }

    if (stage_ == stage::read_value) {
      if (!current_key_.has_value()) {
        ec = make_error_code(error::invalid_format);
        return true;
      }
      if (!child_) {
        child_ = make_message_parser(depth_ + 1);
        if (!child_) {
          ec = make_error_code(error::nesting_too_deep);
          return true;
        }
      }
      message value{};
      std::error_code inner_ec{};
      auto done = child_->parse(buf, value, inner_ec);
      if (!done) {
        return false;
      }
      if (inner_ec) {
        ec = inner_ec;
        return true;
      }
      entries_.push_back({std::move(*current_key_), std::move(value)});
      current_key_.reset();
      child_.reset();
      stage_ = stage::read_key;
      continue;
    }
  }
}

auto attribute_value_parser::parse(buffer& buf, attribute& out, std::error_code& ec) -> bool {
  while (true) {
    if (stage_ == stage::read_len) {
      auto data = buf.data();
      auto pos = find_crlf(data);
      if (pos == std::string_view::npos) {
        return false;
      }
      if (data.empty() || data[0] != '|') {
        ec = make_error_code(error::invalid_format);
        return true;
      }
      std::int64_t len{};
      if (!parse_len(data.substr(1, pos - 1), len)) {
        ec = make_error_code(error::invalid_length);
        return true;
      }
      if (len < 0) {
        ec = make_error_code(error::invalid_length);
        return true;
      }
      buf.consume(pos + 2);
      expected_ = len;
      entries_.clear();
      entries_.reserve(static_cast<std::size_t>(expected_));
      current_key_.reset();
      stage_ = stage::read_key;
      if (expected_ == 0) {
        out = attribute{std::move(entries_)};
        return true;
      }
    }

    if (stage_ == stage::read_key) {
      if (entries_.size() >= static_cast<std::size_t>(expected_)) {
        out = attribute{std::move(entries_)};
        return true;
      }
      if (!child_) {
        child_ = make_message_parser(depth_ + 1);
        if (!child_) {
          ec = make_error_code(error::nesting_too_deep);
          return true;
        }
      }
      message key{};
      std::error_code inner_ec{};
      auto done = child_->parse(buf, key, inner_ec);
      if (!done) {
        return false;
      }
      if (inner_ec) {
        ec = inner_ec;
        return true;
      }
      current_key_ = std::move(key);
      child_.reset();
      stage_ = stage::read_value;
      continue;
    }

    if (stage_ == stage::read_value) {
      if (!current_key_.has_value()) {
        ec = make_error_code(error::invalid_format);
        return true;
      }
      if (!child_) {
        child_ = make_message_parser(depth_ + 1);
        if (!child_) {
          ec = make_error_code(error::nesting_too_deep);
          return true;
        }
      }
      message value{};
      std::error_code inner_ec{};
      auto done = child_->parse(buf, value, inner_ec);
      if (!done) {
        return false;
      }
      if (inner_ec) {
        ec = inner_ec;
        return true;
      }
      entries_.push_back({std::move(*current_key_), std::move(value)});
      current_key_.reset();
      child_.reset();
      stage_ = stage::read_key;
      continue;
    }
  }
}

}  // namespace rediscoro::resp3::detail

namespace rediscoro::resp3 {

inline parser::parser() = default;

inline auto parser::feed(std::string_view data) -> void {
  if (data.empty()) {
    return;
  }
  auto writable = buffer_.prepare(data.size());
  std::memcpy(writable.data(), data.data(), data.size());
  buffer_.commit(data.size());
}

inline auto parser::parse_one(message& out) -> parse_result {
  if (state_ == state::failed) {
    return parse_result{parse_status::protocol_error, last_error_};
  }

  if (!current_) {
    current_ = detail::make_message_parser(0);
    if (!current_) {
      state_ = state::failed;
      last_error_ = make_error_code(error::nesting_too_deep);
      return parse_result{parse_status::protocol_error, last_error_};
    }
  }

  state_ = state::parsing;

  message tmp{};
  std::error_code ec{};
  auto done = current_->parse(buffer_, tmp, ec);
  if (!done) {
    state_ = state::idle;
    return parse_result{parse_status::need_more_data, {}};
  }

  current_.reset();

  if (ec) {
    state_ = state::failed;
    last_error_ = ec;
    return parse_result{parse_status::protocol_error, ec};
  }

  out = std::move(tmp);
  state_ = state::idle;
  return parse_result{parse_status::ok, {}};
}

inline auto parser::failed() const noexcept -> bool {
  return state_ == state::failed;
}

inline auto parser::reset() -> void {
  buffer_.reset();
  current_.reset();
  last_error_.clear();
  state_ = state::idle;
}

}  // namespace rediscoro::resp3


