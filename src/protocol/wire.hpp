// Protobuf-style wire encoding for JRP/1 metadata: varints and tag-length-value
// fields. Only the encoding is borrowed from Protocol Buffers; there is no
// schema compiler and no dependency on the protobuf library.
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace jrelay::wire {

enum class WireType : std::uint8_t {
  Varint = 0,  // unsigned 64-bit integer, 1 to 10 bytes
  I64 = 1,     // fixed 8 bytes, little-endian (accepted and skipped; unused today)
  Len = 2,     // varint length followed by that many bytes
};

enum class DecodeError {
  Truncated,        // input ended in the middle of a value
  VarintOverflow,   // more than 64 bits encoded
  BadWireType,      // wire type not in {0, 1, 2}
  BadFieldNumber,   // field number 0 or above 2^29 - 1
};

std::string_view to_string(DecodeError e) noexcept;

inline constexpr std::uint32_t kMaxFieldNumber = (1u << 29) - 1;
inline constexpr std::size_t kMaxVarintBytes = 10;

// Appends the varint encoding of `value` to `out`.
void put_varint(std::string& out, std::uint64_t value);

// Reads a varint from the front of `in` and advances `in` past it.
std::expected<std::uint64_t, DecodeError> get_varint(std::string_view& in) noexcept;

// Builds a metadata frame field by field.
class Writer {
 public:
  void varint(std::uint32_t field, std::uint64_t value);
  void bytes(std::uint32_t field, std::string_view value);

  [[nodiscard]] const std::string& data() const& noexcept { return buffer_; }
  [[nodiscard]] std::string take() && noexcept { return std::move(buffer_); }

 private:
  void tag(std::uint32_t field, WireType type);
  std::string buffer_;
};

struct Field {
  std::uint32_t number{};
  WireType type{};
  std::uint64_t integer{};   // valid for Varint and I64
  std::string_view bytes{};  // valid for Len; points into the reader's input
};

// Iterates over the fields of a metadata frame. The returned views stay valid
// as long as the input buffer does.
class Reader {
 public:
  explicit Reader(std::string_view input) noexcept : rest_(input) {}

  // Returns the next field, std::nullopt at the end, or a decode error.
  std::expected<std::optional<Field>, DecodeError> next() noexcept;

 private:
  std::string_view rest_;
};

}  // namespace jrelay::wire
