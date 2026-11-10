#include "common/digest.hpp"

namespace jrelay {

Hasher::Hasher() noexcept { blake3_hasher_init(&state_); }

void Hasher::update(std::span<const std::byte> data) noexcept {
  blake3_hasher_update(&state_, data.data(), data.size());
}

void Hasher::update(const void* data, std::size_t size) noexcept {
  blake3_hasher_update(&state_, data, size);
}

proto::Digest Hasher::finish() const noexcept {
  proto::Digest out{};
  blake3_hasher_finalize(&state_, out.data(), out.size());
  return out;
}

std::string to_hex(const proto::Digest& d) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string s;
  s.reserve(d.size() * 2);
  for (auto b : d) {
    s.push_back(kHex[b >> 4]);
    s.push_back(kHex[b & 0xF]);
  }
  return s;
}

}  // namespace jrelay
