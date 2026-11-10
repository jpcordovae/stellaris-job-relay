#include "protocol/wire.hpp"

#include <cstring>

namespace jrelay::wire {

std::string_view to_string(DecodeError e) noexcept {
  switch (e) {
    case DecodeError::Truncated: return "truncated input";
    case DecodeError::VarintOverflow: return "varint overflow";
    case DecodeError::BadWireType: return "unsupported wire type";
    case DecodeError::BadFieldNumber: return "invalid field number";
  }
  return "unknown decode error";
}

void put_varint(std::string& out, std::uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<char>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<char>(value));
}

std::expected<std::uint64_t, DecodeError> get_varint(std::string_view& in) noexcept {
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < kMaxVarintBytes; ++i) {
    if (i >= in.size()) return std::unexpected(DecodeError::Truncated);
    const auto byte = static_cast<std::uint8_t>(in[i]);
    // The 10th byte may only carry the single remaining bit of a 64-bit value.
    if (i == kMaxVarintBytes - 1 && byte > 1) return std::unexpected(DecodeError::VarintOverflow);
    result |= static_cast<std::uint64_t>(byte & 0x7F) << (7 * i);
    if ((byte & 0x80) == 0) {
      in.remove_prefix(i + 1);
      return result;
    }
  }
  return std::unexpected(DecodeError::VarintOverflow);
}

void Writer::tag(std::uint32_t field, WireType type) {
  put_varint(buffer_, (static_cast<std::uint64_t>(field) << 3) | static_cast<std::uint64_t>(type));
}

void Writer::varint(std::uint32_t field, std::uint64_t value) {
  tag(field, WireType::Varint);
  put_varint(buffer_, value);
}

void Writer::bytes(std::uint32_t field, std::string_view value) {
  tag(field, WireType::Len);
  put_varint(buffer_, value.size());
  buffer_.append(value);
}

std::expected<std::optional<Field>, DecodeError> Reader::next() noexcept {
  if (rest_.empty()) return std::optional<Field>{};

  auto key = get_varint(rest_);
  if (!key) return std::unexpected(key.error());

  const std::uint64_t number = *key >> 3;
  if (number == 0 || number > kMaxFieldNumber) return std::unexpected(DecodeError::BadFieldNumber);

  Field field;
  field.number = static_cast<std::uint32_t>(number);
  switch (*key & 0x7) {
    case 0: {
      field.type = WireType::Varint;
      auto v = get_varint(rest_);
      if (!v) return std::unexpected(v.error());
      field.integer = *v;
      break;
    }
    case 1: {
      field.type = WireType::I64;
      if (rest_.size() < 8) return std::unexpected(DecodeError::Truncated);
      std::uint64_t v = 0;
      for (std::size_t i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(rest_[i])) << (8 * i);
      }
      field.integer = v;
      rest_.remove_prefix(8);
      break;
    }
    case 2: {
      field.type = WireType::Len;
      auto len = get_varint(rest_);
      if (!len) return std::unexpected(len.error());
      if (*len > rest_.size()) return std::unexpected(DecodeError::Truncated);
      field.bytes = rest_.substr(0, static_cast<std::size_t>(*len));
      rest_.remove_prefix(static_cast<std::size_t>(*len));
      break;
    }
    default:
      return std::unexpected(DecodeError::BadWireType);
  }
  return std::optional<Field>{field};
}

}  // namespace jrelay::wire
