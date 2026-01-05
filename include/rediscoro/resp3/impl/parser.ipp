#pragma once

#include <charconv>
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
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return false;
    }
    if (data.empty() || data[0] != type_to_code(type::simple_string)) {
      err = error::invalid_format;
      return true;
    }
    out = message(simple_string{std::string(data.substr(1, pos - 1))});
    buf.consume(pos + 2);
    return true;
  }
};

class simple_error_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return false;
    }
    if (data.empty() || data[0] != type_to_code(type::simple_error)) {
      err = error::invalid_format;
      return true;
    }
    out = message(simple_error{std::string(data.substr(1, pos - 1))});
    buf.consume(pos + 2);
    return true;
  }
};

class integer_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return false;
    }
    if (data.empty() || data[0] != type_to_code(type::integer)) {
      err = error::invalid_format;
      return true;
    }
    std::int64_t v{};
    if (!parse_i64(data.substr(1, pos - 1), v)) {
      err = error::invalid_integer;
      return true;
    }
    out = message(integer{v});
    buf.consume(pos + 2);
    return true;
  }
};

class double_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return false;
    }
    if (data.empty() || data[0] != type_to_code(type::double_type)) {
      err = error::invalid_format;
      return true;
    }
    double v{};
    if (!parse_double(data.substr(1, pos - 1), v)) {
      err = error::invalid_format;
      return true;
    }
    out = message(double_type{v});
    buf.consume(pos + 2);
    return true;
  }
};

class boolean_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override {
    auto data = buf.data();
    if (data.size() < 4) {
      return false;
    }
    if (data[0] != type_to_code(type::boolean)) {
      err = error::invalid_format;
      return true;
    }
    if (data[3] != '\n' || data[2] != '\r') {
      err = error::invalid_format;
      return true;
    }
    if (data[1] == 't') {
      out = message(boolean{true});
    } else if (data[1] == 'f') {
      out = message(boolean{false});
    } else {
      err = error::invalid_format;
      return true;
    }
    buf.consume(4);
    return true;
  }
};

class big_number_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return false;
    }
    if (data.empty() || data[0] != type_to_code(type::big_number)) {
      err = error::invalid_format;
      return true;
    }
    out = message(big_number{std::string(data.substr(1, pos - 1))});
    buf.consume(pos + 2);
    return true;
  }
};

class null_parser final : public value_parser {
public:
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override {
    auto data = buf.data();
    if (data.size() < 3) {
      return false;
    }
    if (data[0] != type_to_code(type::null)) {
      err = error::invalid_format;
      return true;
    }
    if (data[1] != '\r' || data[2] != '\n') {
      err = error::invalid_format;
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
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override {
    while (true) {
      if (stage_ == stage::read_len) {
        auto data = buf.data();
        auto pos = find_crlf(data);
        if (pos == std::string_view::npos) {
          return false;
        }
        if (data.empty() || data[0] != type_to_code(type::bulk_string)) {
          err = error::invalid_format;
          return true;
        }
        std::int64_t len{};
        if (!parse_len(data.substr(1, pos - 1), len)) {
          err = error::invalid_length;
          return true;
        }
        if (len < -1) {
          err = error::invalid_length;
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
          err = error::invalid_format;
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
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override {
    while (true) {
      if (stage_ == stage::read_len) {
        auto data = buf.data();
        auto pos = find_crlf(data);
        if (pos == std::string_view::npos) {
          return false;
        }
        if (data.empty() || data[0] != type_to_code(type::bulk_error)) {
          err = error::invalid_format;
          return true;
        }
        std::int64_t len{};
        if (!parse_len(data.substr(1, pos - 1), len)) {
          err = error::invalid_length;
          return true;
        }
        if (len < 0) {
          err = error::invalid_length;
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
          err = error::invalid_format;
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
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override {
    while (true) {
      if (stage_ == stage::read_len) {
        auto data = buf.data();
        auto pos = find_crlf(data);
        if (pos == std::string_view::npos) {
          return false;
        }
        if (data.empty() || data[0] != type_to_code(type::verbatim_string)) {
          err = error::invalid_format;
          return true;
        }
        std::int64_t len{};
        if (!parse_len(data.substr(1, pos - 1), len)) {
          err = error::invalid_length;
          return true;
        }
        if (len < -1) {
          err = error::invalid_length;
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
          err = error::invalid_format;
          return true;
        }
        auto payload = data.substr(0, static_cast<std::size_t>(expected_));
        if (payload.size() < 4) {
          err = error::invalid_format;
          return true;
        }
        if (payload[3] != ':') {
          err = error::invalid_format;
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

  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override;
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
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override;
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
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override;
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
  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override;
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
  auto parse(buffer& buf, attribute& out, std::optional<error>& err) -> bool;
};

[[nodiscard]] auto make_message_parser(std::size_t depth) -> std::unique_ptr<value_parser>;

[[nodiscard]] auto make_value_parser_for_type(char type_byte, std::size_t depth, std::optional<error>& err)
  -> std::unique_ptr<value_parser> {
  if (depth > max_nesting_depth) {
    err = error::nesting_too_deep;
    return nullptr;
  }

  auto maybe_t = code_to_type(type_byte);
  if (!maybe_t.has_value()) {
    err = error::invalid_type_byte;
    return nullptr;
  }

  switch (*maybe_t) {
    case type::simple_string:   return std::make_unique<simple_string_parser>();
    case type::simple_error:    return std::make_unique<simple_error_parser>();
    case type::integer:         return std::make_unique<integer_parser>();
    case type::double_type:     return std::make_unique<double_parser>();
    case type::boolean:         return std::make_unique<boolean_parser>();
    case type::big_number:      return std::make_unique<big_number_parser>();
    case type::null:            return std::make_unique<null_parser>();
    case type::bulk_string:     return std::make_unique<bulk_string_parser>();
    case type::bulk_error:      return std::make_unique<bulk_error_parser>();
    case type::verbatim_string: return std::make_unique<verbatim_string_parser>();
    case type::array:           return std::make_unique<array_parser>(depth);
    case type::map:             return std::make_unique<map_parser>(depth);
    case type::set:             return std::make_unique<set_parser>(depth);
    case type::push:            return std::make_unique<push_parser>(depth);
    case type::attribute:
      // Attributes are handled as a prefix by message_parser, never as a "value".
      err = error::invalid_format;
      return nullptr;
  }

  err = error::invalid_type_byte;
  return nullptr;
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

  auto parse(buffer& buf, message& out, std::optional<error>& err) -> bool override {
    while (true) {
      auto data = buf.data();
      if (data.empty()) {
        return false;
      }

      if (stage_ == stage::read_attrs) {
        if (data[0] == type_to_code(type::attribute)) {
          if (!attr_child_) {
            attr_child_ = std::make_unique<attribute_value_parser>(depth_);
          }
          attribute tmp_attr{};
          std::optional<error> inner_err{};
          auto done = attr_child_->parse(buf, tmp_attr, inner_err);
          if (!done) {
            return false;
          }
          if (inner_err.has_value()) {
            err = *inner_err;
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
          std::optional<error> make_err{};
          if (data[0] == type_to_code(type::attribute)) {
            stage_ = stage::read_attrs;
            continue;
          }
          child_ = make_value_parser_for_type(data[0], depth_, make_err);
          if (make_err.has_value()) {
            err = *make_err;
            return true;
          }
          if (!child_) {
            err = error::invalid_format;
            return true;
          }
        }

        message value_msg{};
        std::optional<error> inner_err{};
        auto done = child_->parse(buf, value_msg, inner_err);
        if (!done) {
          return false;
        }
        if (inner_err.has_value()) {
          err = *inner_err;
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

auto array_parser::parse(buffer& buf, message& out, std::optional<error>& err) -> bool {
  while (true) {
    if (stage_ == stage::read_len) {
      auto data = buf.data();
      auto pos = find_crlf(data);
      if (pos == std::string_view::npos) {
        return false;
      }
      if (data.empty() || data[0] != type_to_code(type::array)) {
        err = error::invalid_format;
        return true;
      }
      std::int64_t len{};
      if (!parse_len(data.substr(1, pos - 1), len)) {
        err = error::invalid_length;
        return true;
      }
      if (len < -1) {
        err = error::invalid_length;
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
            err = error::nesting_too_deep;
            return true;
          }
        }
        message elem{};
        std::optional<error> inner_err{};
        auto done = child_->parse(buf, elem, inner_err);
        if (!done) {
          return false;
        }
        if (inner_err.has_value()) {
          err = *inner_err;
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

auto set_parser::parse(buffer& buf, message& out, std::optional<error>& err) -> bool {
  while (true) {
    if (stage_ == stage::read_len) {
      auto data = buf.data();
      auto pos = find_crlf(data);
      if (pos == std::string_view::npos) {
        return false;
      }
      if (data.empty() || data[0] != type_to_code(type::set)) {
        err = error::invalid_format;
        return true;
      }
      std::int64_t len{};
      if (!parse_len(data.substr(1, pos - 1), len)) {
        err = error::invalid_length;
        return true;
      }
      if (len < -1) {
        err = error::invalid_length;
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
            err = error::nesting_too_deep;
            return true;
          }
        }
        message elem{};
        std::optional<error> inner_err{};
        auto done = child_->parse(buf, elem, inner_err);
        if (!done) {
          return false;
        }
        if (inner_err.has_value()) {
          err = *inner_err;
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

auto push_parser::parse(buffer& buf, message& out, std::optional<error>& err) -> bool {
  while (true) {
    if (stage_ == stage::read_len) {
      auto data = buf.data();
      auto pos = find_crlf(data);
      if (pos == std::string_view::npos) {
        return false;
      }
      if (data.empty() || data[0] != type_to_code(type::push)) {
        err = error::invalid_format;
        return true;
      }
      std::int64_t len{};
      if (!parse_len(data.substr(1, pos - 1), len)) {
        err = error::invalid_length;
        return true;
      }
      if (len < -1) {
        err = error::invalid_length;
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
            err = error::nesting_too_deep;
            return true;
          }
        }
        message elem{};
        std::optional<error> inner_err{};
        auto done = child_->parse(buf, elem, inner_err);
        if (!done) {
          return false;
        }
        if (inner_err.has_value()) {
          err = *inner_err;
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

auto map_parser::parse(buffer& buf, message& out, std::optional<error>& err) -> bool {
  while (true) {
    if (stage_ == stage::read_len) {
      auto data = buf.data();
      auto pos = find_crlf(data);
      if (pos == std::string_view::npos) {
        return false;
      }
      if (data.empty() || data[0] != type_to_code(type::map)) {
        err = error::invalid_format;
        return true;
      }
      std::int64_t len{};
      if (!parse_len(data.substr(1, pos - 1), len)) {
        err = error::invalid_length;
        return true;
      }
      if (len < -1) {
        err = error::invalid_length;
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
          err = error::nesting_too_deep;
          return true;
        }
      }
      message key{};
      std::optional<error> inner_err{};
      auto done = child_->parse(buf, key, inner_err);
      if (!done) {
        return false;
      }
      if (inner_err.has_value()) {
        err = *inner_err;
        return true;
      }
      current_key_ = std::move(key);
      child_.reset();
      stage_ = stage::read_value;
      continue;
    }

    if (stage_ == stage::read_value) {
      if (!current_key_.has_value()) {
        err = error::invalid_format;
        return true;
      }
      if (!child_) {
        child_ = make_message_parser(depth_ + 1);
        if (!child_) {
          err = error::nesting_too_deep;
          return true;
        }
      }
      message value{};
      std::optional<error> inner_err{};
      auto done = child_->parse(buf, value, inner_err);
      if (!done) {
        return false;
      }
      if (inner_err.has_value()) {
        err = *inner_err;
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

auto attribute_value_parser::parse(buffer& buf, attribute& out, std::optional<error>& err) -> bool {
  while (true) {
    if (stage_ == stage::read_len) {
      auto data = buf.data();
      auto pos = find_crlf(data);
      if (pos == std::string_view::npos) {
        return false;
      }
      if (data.empty() || data[0] != type_to_code(type::attribute)) {
        err = error::invalid_format;
        return true;
      }
      std::int64_t len{};
      if (!parse_len(data.substr(1, pos - 1), len)) {
        err = error::invalid_length;
        return true;
      }
      if (len < 0) {
        err = error::invalid_length;
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
          err = error::nesting_too_deep;
          return true;
        }
      }
      message key{};
      std::optional<error> inner_err{};
      auto done = child_->parse(buf, key, inner_err);
      if (!done) {
        return false;
      }
      if (inner_err.has_value()) {
        err = *inner_err;
        return true;
      }
      current_key_ = std::move(key);
      child_.reset();
      stage_ = stage::read_value;
      continue;
    }

    if (stage_ == stage::read_value) {
      if (!current_key_.has_value()) {
        err = error::invalid_format;
        return true;
      }
      if (!child_) {
        child_ = make_message_parser(depth_ + 1);
        if (!child_) {
          err = error::nesting_too_deep;
          return true;
        }
      }
      message value{};
      std::optional<error> inner_err{};
      auto done = child_->parse(buf, value, inner_err);
      if (!done) {
        return false;
      }
      if (inner_err.has_value()) {
        err = *inner_err;
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

inline auto parser::prepare(std::size_t min_size) -> std::span<char> {
  return buffer_.prepare(min_size);
}

inline auto parser::commit(std::size_t n) -> void {
  buffer_.commit(n);
}

inline auto parser::parse_one() -> rediscoro::expected<message, error> {
  if (failed_) {
    return rediscoro::unexpected(failed_error_);
  }

  if (!current_) {
    current_ = detail::make_message_parser(0);
    if (!current_) {
      failed_ = true;
      failed_error_ = error::nesting_too_deep;
      return rediscoro::unexpected(failed_error_);
    }
  }

  message tmp{};
  std::optional<error> err{};
  auto done = current_->parse(buffer_, tmp, err);
  if (!done) {
    return rediscoro::unexpected(error::needs_more);
  }

  current_.reset();

  if (err.has_value()) {
    failed_ = true;
    failed_error_ = *err;
    return rediscoro::unexpected(failed_error_);
  }

  return tmp;
}

inline auto parser::failed() const noexcept -> bool {
  return failed_;
}

inline auto parser::reset() -> void {
  buffer_.reset();
  current_.reset();
  failed_ = false;
  failed_error_ = error::invalid_format;
}

}  // namespace rediscoro::resp3


