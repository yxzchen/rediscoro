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

using parse_expected = expected<message, std::error_code>;

class attribute_value_parser;

[[nodiscard]] auto make_message_parser(std::size_t depth) -> std::unique_ptr<value_parser>;
[[nodiscard]] auto make_value_body_parser(type t, std::size_t depth) -> std::unique_ptr<value_parser>;

class simple_string_body_parser final : public value_parser {
public:
  auto parse(buffer& buf) -> parse_expected override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return unexpected(error::needs_more);
    }
    auto line = data.substr(0, pos);
    buf.consume(pos + 2);
    return message(simple_string{std::string(line)});
  }
};

class simple_error_body_parser final : public value_parser {
public:
  auto parse(buffer& buf) -> parse_expected override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return unexpected(error::needs_more);
    }
    auto line = data.substr(0, pos);
    buf.consume(pos + 2);
    return message(simple_error{std::string(line)});
  }
};

class integer_body_parser final : public value_parser {
public:
  auto parse(buffer& buf) -> parse_expected override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return unexpected(error::needs_more);
    }
    std::int64_t v{};
    if (!parse_i64(data.substr(0, pos), v)) {
      return unexpected(error::invalid_integer);
    }
    buf.consume(pos + 2);
    return message(integer{v});
  }
};

class double_body_parser final : public value_parser {
public:
  auto parse(buffer& buf) -> parse_expected override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return unexpected(error::needs_more);
    }
    double v{};
    if (!parse_double(data.substr(0, pos), v)) {
      return unexpected(error::invalid_format);
    }
    buf.consume(pos + 2);
    return message(double_type{v});
  }
};

class boolean_body_parser final : public value_parser {
public:
  auto parse(buffer& buf) -> parse_expected override {
    auto data = buf.data();
    if (data.size() < 3) {
      return unexpected(error::needs_more);
    }
    if (data[1] != '\r' || data[2] != '\n') {
      return unexpected(error::invalid_format);
    }
    if (data[0] == 't') {
      buf.consume(3);
      return message(boolean{true});
    }
    if (data[0] == 'f') {
      buf.consume(3);
      return message(boolean{false});
    }
    return unexpected(error::invalid_format);
  }
};

class big_number_body_parser final : public value_parser {
public:
  auto parse(buffer& buf) -> parse_expected override {
    auto data = buf.data();
    auto pos = find_crlf(data);
    if (pos == std::string_view::npos) {
      return unexpected(error::needs_more);
    }
    auto line = data.substr(0, pos);
    buf.consume(pos + 2);
    return message(big_number{std::string(line)});
  }
};

class null_body_parser final : public value_parser {
public:
  auto parse(buffer& buf) -> parse_expected override {
    auto data = buf.data();
    if (data.size() < 2) {
      return unexpected(error::needs_more);
    }
    if (data[0] != '\r' || data[1] != '\n') {
      return unexpected(error::invalid_format);
    }
    buf.consume(2);
    return message(null{});
  }
};

class bulk_string_body_parser final : public value_parser {
  enum class stage { read_len, read_data };
  stage stage_{stage::read_len};
  std::int64_t expected_{0};

public:
  auto parse(buffer& buf) -> parse_expected override {
    while (true) {
      switch (stage_) {
        case stage::read_len: {
          auto data = buf.data();
          auto pos = find_crlf(data);
          if (pos == std::string_view::npos) {
            return unexpected(error::needs_more);
          }
          std::int64_t len{};
          if (!parse_i64(data.substr(0, pos), len)) {
            return unexpected(error::invalid_length);
          }
          if (len < -1) {
            return unexpected(error::invalid_length);
          }
          buf.consume(pos + 2);
          if (len == -1) {
            return message(null{});
          }
          expected_ = len;
          stage_ = stage::read_data;
          continue;
        }
        case stage::read_data: {
          auto data = buf.data();
          auto need = static_cast<std::size_t>(expected_) + 2;
          if (data.size() < need) {
            return unexpected(error::needs_more);
          }
          if (data.substr(static_cast<std::size_t>(expected_), 2) != "\r\n") {
            return unexpected(error::invalid_format);
          }
          auto payload = data.substr(0, static_cast<std::size_t>(expected_));
          buf.consume(need);
          return message(bulk_string{std::string(payload)});
        }
      }
    }
  }
};

class bulk_error_body_parser final : public value_parser {
  enum class stage { read_len, read_data };
  stage stage_{stage::read_len};
  std::int64_t expected_{0};

public:
  auto parse(buffer& buf) -> parse_expected override {
    while (true) {
      switch (stage_) {
        case stage::read_len: {
          auto data = buf.data();
          auto pos = find_crlf(data);
          if (pos == std::string_view::npos) {
            return unexpected(error::needs_more);
          }
          std::int64_t len{};
          if (!parse_i64(data.substr(0, pos), len)) {
            return unexpected(error::invalid_length);
          }
          if (len < 0) {
            return unexpected(error::invalid_length);
          }
          buf.consume(pos + 2);
          expected_ = len;
          stage_ = stage::read_data;
          continue;
        }
        case stage::read_data: {
          auto data = buf.data();
          auto need = static_cast<std::size_t>(expected_) + 2;
          if (data.size() < need) {
            return unexpected(error::needs_more);
          }
          if (data.substr(static_cast<std::size_t>(expected_), 2) != "\r\n") {
            return unexpected(error::invalid_format);
          }
          auto payload = data.substr(0, static_cast<std::size_t>(expected_));
          buf.consume(need);
          return message(bulk_error{std::string(payload)});
        }
      }
    }
  }
};

class verbatim_string_body_parser final : public value_parser {
  enum class stage { read_len, read_data };
  stage stage_{stage::read_len};
  std::int64_t expected_{0};

public:
  auto parse(buffer& buf) -> parse_expected override {
    while (true) {
      switch (stage_) {
        case stage::read_len: {
          auto data = buf.data();
          auto pos = find_crlf(data);
          if (pos == std::string_view::npos) {
            return unexpected(error::needs_more);
          }
          std::int64_t len{};
          if (!parse_i64(data.substr(0, pos), len)) {
            return unexpected(error::invalid_length);
          }
          if (len < -1) {
            return unexpected(error::invalid_length);
          }
          buf.consume(pos + 2);
          if (len == -1) {
            return message(null{});
          }
          expected_ = len;
          stage_ = stage::read_data;
          continue;
        }
        case stage::read_data: {
          auto data = buf.data();
          auto need = static_cast<std::size_t>(expected_) + 2;
          if (data.size() < need) {
            return unexpected(error::needs_more);
          }
          if (data.substr(static_cast<std::size_t>(expected_), 2) != "\r\n") {
            return unexpected(error::invalid_format);
          }
          auto payload = data.substr(0, static_cast<std::size_t>(expected_));
          if (payload.size() < 4) {
            return unexpected(error::invalid_format);
          }
          if (payload[3] != ':') {
            return unexpected(error::invalid_format);
          }
          verbatim_string v{};
          v.encoding = std::string(payload.substr(0, 3));
          v.data = std::string(payload.substr(4));
          buf.consume(need);
          return message(std::move(v));
        }
      }
    }
  }
};

class message_parser final : public value_parser {
  enum class stage { read_attrs, read_type, read_value };

  stage stage_{stage::read_attrs};
  std::optional<attribute> attrs_{};
  std::unique_ptr<value_parser> child_{};
  std::unique_ptr<attribute_value_parser> attr_child_{};
  std::size_t depth_{0};

public:
  explicit message_parser(std::size_t depth) : depth_(depth) {}

  auto parse(buffer& buf) -> parse_expected override;
};

using attr_expected = expected<attribute, std::error_code>;

class attribute_value_parser final {
  enum class stage { read_len, read_key, read_value };

  stage stage_{stage::read_len};
  std::int64_t expected_{0};
  std::vector<std::pair<message, message>> entries_{};
  std::unique_ptr<value_parser> child_{};
  std::optional<message> current_key_{};
  std::size_t depth_{0};

public:
  explicit attribute_value_parser(std::size_t depth) : depth_(depth) {}

  auto parse(buffer& buf) -> attr_expected {
    while (true) {
      switch (stage_) {
        case stage::read_len: {
          auto data = buf.data();
          auto pos = find_crlf(data);
          if (pos == std::string_view::npos) {
            return unexpected(error::needs_more);
          }
          std::int64_t len{};
          if (!parse_i64(data.substr(0, pos), len)) {
            return unexpected(error::invalid_length);
          }
          if (len < 0) {
            return unexpected(error::invalid_length);
          }
          buf.consume(pos + 2);
          expected_ = len;
          entries_.clear();
          entries_.reserve(static_cast<std::size_t>(expected_));
          current_key_.reset();
          stage_ = stage::read_key;
          if (expected_ == 0) {
            attribute out{};
            out.entries = std::move(entries_);
            return out;
          }
          continue;
        }
        case stage::read_key: {
          if (entries_.size() >= static_cast<std::size_t>(expected_)) {
            attribute out{};
            out.entries = std::move(entries_);
            return out;
          }
          if (!child_) {
            child_ = make_message_parser(depth_ + 1);
            if (!child_) {
              return unexpected(error::nesting_too_deep);
            }
          }
          auto r = child_->parse(buf);
          if (!r) {
            return unexpected(r.error());
          }
          current_key_ = std::move(*r);
          child_.reset();
          stage_ = stage::read_value;
          continue;
        }
        case stage::read_value: {
          if (!current_key_.has_value()) {
            return unexpected(error::invalid_format);
          }
          if (!child_) {
            child_ = make_message_parser(depth_ + 1);
            if (!child_) {
              return unexpected(error::nesting_too_deep);
            }
          }
          auto r = child_->parse(buf);
          if (!r) {
            return unexpected(r.error());
          }
          entries_.push_back({std::move(*current_key_), std::move(*r)});
          current_key_.reset();
          child_.reset();
          stage_ = stage::read_key;
          continue;
        }
      }
    }
  }
};

class array_body_parser final : public value_parser {
  enum class stage { read_len, read_elements };
  stage stage_{stage::read_len};
  std::int64_t expected_{0};
  std::vector<message> elements_{};
  std::unique_ptr<value_parser> child_{};
  std::size_t depth_{0};

public:
  explicit array_body_parser(std::size_t depth) : depth_(depth) {}

  auto parse(buffer& buf) -> parse_expected override {
    while (true) {
      switch (stage_) {
        case stage::read_len: {
          auto data = buf.data();
          auto pos = find_crlf(data);
          if (pos == std::string_view::npos) {
            return unexpected(error::needs_more);
          }
          std::int64_t len{};
          if (!parse_i64(data.substr(0, pos), len)) {
            return unexpected(error::invalid_length);
          }
          if (len < -1) {
            return unexpected(error::invalid_length);
          }
          buf.consume(pos + 2);
          if (len == -1) {
            return message(null{});
          }
          expected_ = len;
          elements_.clear();
          elements_.reserve(static_cast<std::size_t>(expected_));
          stage_ = stage::read_elements;
          if (expected_ == 0) {
            return message(array{std::move(elements_)});
          }
          continue;
        }
        case stage::read_elements: {
          while (elements_.size() < static_cast<std::size_t>(expected_)) {
            if (!child_) {
              child_ = make_message_parser(depth_ + 1);
              if (!child_) {
                return unexpected(error::nesting_too_deep);
              }
            }
            auto r = child_->parse(buf);
            if (!r) {
              return unexpected(r.error());
            }
            elements_.push_back(std::move(*r));
            child_.reset();
          }
          return message(array{std::move(elements_)});
        }
      }
    }
  }
};

class set_body_parser final : public value_parser {
  enum class stage { read_len, read_elements };
  stage stage_{stage::read_len};
  std::int64_t expected_{0};
  std::vector<message> elements_{};
  std::unique_ptr<value_parser> child_{};
  std::size_t depth_{0};

public:
  explicit set_body_parser(std::size_t depth) : depth_(depth) {}

  auto parse(buffer& buf) -> parse_expected override {
    while (true) {
      switch (stage_) {
        case stage::read_len: {
          auto data = buf.data();
          auto pos = find_crlf(data);
          if (pos == std::string_view::npos) {
            return unexpected(error::needs_more);
          }
          std::int64_t len{};
          if (!parse_i64(data.substr(0, pos), len)) {
            return unexpected(error::invalid_length);
          }
          if (len < -1) {
            return unexpected(error::invalid_length);
          }
          buf.consume(pos + 2);
          if (len == -1) {
            return message(null{});
          }
          expected_ = len;
          elements_.clear();
          elements_.reserve(static_cast<std::size_t>(expected_));
          stage_ = stage::read_elements;
          if (expected_ == 0) {
            return message(set{std::move(elements_)});
          }
          continue;
        }
        case stage::read_elements: {
          while (elements_.size() < static_cast<std::size_t>(expected_)) {
            if (!child_) {
              child_ = make_message_parser(depth_ + 1);
              if (!child_) {
                return unexpected(error::nesting_too_deep);
              }
            }
            auto r = child_->parse(buf);
            if (!r) {
              return unexpected(r.error());
            }
            elements_.push_back(std::move(*r));
            child_.reset();
          }
          return message(set{std::move(elements_)});
        }
      }
    }
  }
};

class push_body_parser final : public value_parser {
  enum class stage { read_len, read_elements };
  stage stage_{stage::read_len};
  std::int64_t expected_{0};
  std::vector<message> elements_{};
  std::unique_ptr<value_parser> child_{};
  std::size_t depth_{0};

public:
  explicit push_body_parser(std::size_t depth) : depth_(depth) {}

  auto parse(buffer& buf) -> parse_expected override {
    while (true) {
      switch (stage_) {
        case stage::read_len: {
          auto data = buf.data();
          auto pos = find_crlf(data);
          if (pos == std::string_view::npos) {
            return unexpected(error::needs_more);
          }
          std::int64_t len{};
          if (!parse_i64(data.substr(0, pos), len)) {
            return unexpected(error::invalid_length);
          }
          if (len < -1) {
            return unexpected(error::invalid_length);
          }
          buf.consume(pos + 2);
          if (len == -1) {
            return message(null{});
          }
          expected_ = len;
          elements_.clear();
          elements_.reserve(static_cast<std::size_t>(expected_));
          stage_ = stage::read_elements;
          if (expected_ == 0) {
            return message(push{std::move(elements_)});
          }
          continue;
        }
        case stage::read_elements: {
          while (elements_.size() < static_cast<std::size_t>(expected_)) {
            if (!child_) {
              child_ = make_message_parser(depth_ + 1);
              if (!child_) {
                return unexpected(error::nesting_too_deep);
              }
            }
            auto r = child_->parse(buf);
            if (!r) {
              return unexpected(r.error());
            }
            elements_.push_back(std::move(*r));
            child_.reset();
          }
          return message(push{std::move(elements_)});
        }
      }
    }
  }
};

class map_body_parser final : public value_parser {
  enum class stage { read_len, read_key, read_value };
  stage stage_{stage::read_len};
  std::int64_t expected_{0};
  std::vector<std::pair<message, message>> entries_{};
  std::unique_ptr<value_parser> child_{};
  std::optional<message> current_key_{};
  std::size_t depth_{0};

public:
  explicit map_body_parser(std::size_t depth) : depth_(depth) {}

  auto parse(buffer& buf) -> parse_expected override {
    while (true) {
      switch (stage_) {
        case stage::read_len: {
          auto data = buf.data();
          auto pos = find_crlf(data);
          if (pos == std::string_view::npos) {
            return unexpected(error::needs_more);
          }
          std::int64_t len{};
          if (!parse_i64(data.substr(0, pos), len)) {
            return unexpected(error::invalid_length);
          }
          if (len < -1) {
            return unexpected(error::invalid_length);
          }
          buf.consume(pos + 2);
          if (len == -1) {
            return message(null{});
          }
          expected_ = len;
          entries_.clear();
          entries_.reserve(static_cast<std::size_t>(expected_));
          current_key_.reset();
          stage_ = stage::read_key;
          if (expected_ == 0) {
            return message(map{std::move(entries_)});
          }
          continue;
        }
        case stage::read_key: {
          if (entries_.size() >= static_cast<std::size_t>(expected_)) {
            return message(map{std::move(entries_)});
          }
          if (!child_) {
            child_ = make_message_parser(depth_ + 1);
            if (!child_) {
              return unexpected(error::nesting_too_deep);
            }
          }
          auto r = child_->parse(buf);
          if (!r) {
            return unexpected(r.error());
          }
          current_key_ = std::move(*r);
          child_.reset();
          stage_ = stage::read_value;
          continue;
        }
        case stage::read_value: {
          if (!current_key_.has_value()) {
            return unexpected(error::invalid_format);
          }
          if (!child_) {
            child_ = make_message_parser(depth_ + 1);
            if (!child_) {
              return unexpected(error::nesting_too_deep);
            }
          }
          auto r = child_->parse(buf);
          if (!r) {
            return unexpected(r.error());
          }
          entries_.push_back({std::move(*current_key_), std::move(*r)});
          current_key_.reset();
          child_.reset();
          stage_ = stage::read_key;
          continue;
        }
      }
    }
  }
};

auto message_parser::parse(buffer& buf) -> parse_expected {
  while (true) {
    switch (stage_) {
      case stage::read_attrs: {
        auto data = buf.data();
        if (data.empty()) {
          return unexpected(error::needs_more);
        }
        if (data[0] != type_to_code(type::attribute)) {
          stage_ = stage::read_type;
          continue;
        }
        // consume '|' before parsing attribute body
        buf.consume(1);
        if (!attr_child_) {
          attr_child_ = std::make_unique<attribute_value_parser>(depth_);
        }
        auto r = attr_child_->parse(buf);
        if (!r) {
          return unexpected(r.error());
        }
        if (!attrs_.has_value()) {
          attrs_ = attribute{};
        }
        auto& dst = attrs_->entries;
        auto& src = r->entries;
        dst.insert(dst.end(),
                   std::make_move_iterator(src.begin()),
                   std::make_move_iterator(src.end()));
        attr_child_.reset();
        continue;
      }
      case stage::read_type: {
        auto data = buf.data();
        if (data.empty()) {
          return unexpected(error::needs_more);
        }
        auto b = data[0];
        if (b == type_to_code(type::attribute)) {
          stage_ = stage::read_attrs;
          continue;
        }
        buf.consume(1);
        auto maybe_t = code_to_type(b);
        if (!maybe_t.has_value()) {
          return unexpected(error::invalid_type_byte);
        }
        if (depth_ + 1 > max_nesting_depth) {
          return unexpected(error::nesting_too_deep);
        }
        child_ = make_value_body_parser(*maybe_t, depth_ + 1);
        if (!child_) {
          return unexpected(error::invalid_format);
        }
        stage_ = stage::read_value;
        continue;
      }
      case stage::read_value: {
        if (!child_) {
          return unexpected(error::invalid_format);
        }
        auto r = child_->parse(buf);
        if (!r) {
          return unexpected(r.error());
        }
        auto msg = std::move(*r);
        if (attrs_.has_value()) {
          msg.attrs = std::move(attrs_);
        }
        return msg;
      }
    }
  }
}

[[nodiscard]] auto make_message_parser(std::size_t depth) -> std::unique_ptr<value_parser> {
  if (depth > max_nesting_depth) {
    return nullptr;
  }
  return std::make_unique<message_parser>(depth);
}

[[nodiscard]] auto make_value_body_parser(type t, std::size_t depth) -> std::unique_ptr<value_parser> {
  if (depth > max_nesting_depth) {
    return nullptr;
  }
  switch (t) {
    case type::simple_string:   return std::make_unique<simple_string_body_parser>();
    case type::simple_error:    return std::make_unique<simple_error_body_parser>();
    case type::integer:         return std::make_unique<integer_body_parser>();
    case type::double_type:     return std::make_unique<double_body_parser>();
    case type::boolean:         return std::make_unique<boolean_body_parser>();
    case type::big_number:      return std::make_unique<big_number_body_parser>();
    case type::null:            return std::make_unique<null_body_parser>();
    case type::bulk_string:     return std::make_unique<bulk_string_body_parser>();
    case type::bulk_error:      return std::make_unique<bulk_error_body_parser>();
    case type::verbatim_string: return std::make_unique<verbatim_string_body_parser>();
    case type::array:           return std::make_unique<array_body_parser>(depth);
    case type::map:             return std::make_unique<map_body_parser>(depth);
    case type::set:             return std::make_unique<set_body_parser>(depth);
    case type::push:            return std::make_unique<push_body_parser>(depth);
    case type::attribute:
      // Attribute is a prefix handled by message_parser.
      return nullptr;
  }
  return nullptr;
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

inline auto parser::parse_one() -> expected<message, std::error_code> {
  if (failed_) {
    return unexpected(failed_error_);
  }

  if (!current_) {
    current_ = detail::make_message_parser(0);
    if (!current_) {
      failed_ = true;
      failed_error_ = error::nesting_too_deep;
      return unexpected(failed_error_);
    }
  }

  auto r = current_->parse(buffer_);
  if (r) {
    current_.reset();
    return r;
  }

  if (r.error() == error::needs_more) {
    return r;
  }

  failed_ = true;
  failed_error_ = r.error();
  current_.reset();
  return r;
}

inline auto parser::failed() const noexcept -> bool {
  return failed_;
}

inline auto parser::reset() -> void {
  buffer_.reset();
  current_.reset();
  failed_ = false;
  failed_error_.clear();
}

}  // namespace rediscoro::resp3


