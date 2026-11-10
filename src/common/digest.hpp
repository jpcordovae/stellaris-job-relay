// Thin RAII-free wrapper over the official BLAKE3 C implementation.
#pragma once

#include <blake3.h>

#include <span>
#include <string>

#include "protocol/message.hpp"

namespace jrelay {

class Hasher {
 public:
  Hasher() noexcept;
  void update(std::span<const std::byte> data) noexcept;
  void update(const void* data, std::size_t size) noexcept;
  [[nodiscard]] proto::Digest finish() const noexcept;

 private:
  blake3_hasher state_;
};

std::string to_hex(const proto::Digest& d);

}  // namespace jrelay
