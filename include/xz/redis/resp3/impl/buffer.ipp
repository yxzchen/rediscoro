#include <xz/redis/resp3/detail/buffer.hpp>

namespace xz::redis::resp3::detail {

void buffer::compact() {
  if (read_pos_ == 0) return;

  std::size_t readable = write_pos_ - read_pos_;
  if (readable > 0) {
    std::memmove(data_.data(), data_.data() + read_pos_, readable);
  }

  read_pos_ = 0;
  write_pos_ = readable;

  // Shrink vector to reclaim memory, but keep some headroom for future writes
  std::size_t new_size = std::max(write_pos_ + 1024, std::size_t(8192));
  data_.resize(new_size);
  data_.shrink_to_fit();
}

void buffer::ensure_writable(std::size_t n) {
  if (writable_size() < n) {
    std::size_t needed = write_pos_ + n;
    std::size_t new_capacity = data_.size();

    if (new_capacity == 0) new_capacity = 1;
    while (new_capacity < needed) {
      new_capacity *= 2;
    }

    data_.resize(new_capacity);
  }
}

}  // namespace xz::redis::resp3::detail
