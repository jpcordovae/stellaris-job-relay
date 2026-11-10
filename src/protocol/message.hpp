// JRP/1 message model: fixed header, commands, error codes and metadata.
// See docs/protocolo.md for the specification.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace jrelay::proto {

inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 4;  // 'J' 'R' version command

enum class Command : std::uint8_t {
  Hello = 0x01,
  Welcome = 0x02,
  Put = 0x10,
  Data = 0x11,
  Commit = 0x12,
  Get = 0x20,
  Credit = 0x21,
  End = 0x22,
  Release = 0x30,
  Ok = 0x7E,
  Error = 0x7F,
};

std::string_view to_string(Command c) noexcept;

enum class ErrorCode : std::uint32_t {
  None = 0,
  BadRequest = 1,         // malformed or out-of-order message
  Unauthorized = 2,       // missing or invalid token, or HELLO not done
  NotFound = 3,           // no message under that key in this run
  NotReady = 4,           // the producer is still uploading
  RunFinished = 5,        // the run was released; data no longer exists
  NoMemory = 6,           // does not fit in the server's memory budget (D1)
  IntegrityMismatch = 7,  // BLAKE3 digest does not match the data
  TooLarge = 8,           // more data than declared, or above the maximum size
  Busy = 9,               // another upload of the same key is in progress
  Timeout = 10,           // client side: no answer in time
  Internal = 11,
};

std::string_view to_string(ErrorCode c) noexcept;

// Metadata field numbers. They never change and are never reused.
namespace field {
inline constexpr std::uint32_t kJobId = 1;
inline constexpr std::uint32_t kTotalSize = 2;
inline constexpr std::uint32_t kOffset = 3;
inline constexpr std::uint32_t kCredit = 4;
inline constexpr std::uint32_t kDigest = 5;
inline constexpr std::uint32_t kFin = 6;
inline constexpr std::uint32_t kErrorCode = 7;
inline constexpr std::uint32_t kErrorText = 8;
inline constexpr std::uint32_t kToken = 9;
inline constexpr std::uint32_t kStepId = 10;
inline constexpr std::uint32_t kName = 11;
inline constexpr std::uint32_t kChunkSize = 12;
// 13..20 reserved.
}  // namespace field

inline constexpr std::size_t kDigestSize = 32;  // BLAKE3-256
using Digest = std::array<std::uint8_t, kDigestSize>;

struct Metadata {
  std::optional<std::string> job_id;
  std::optional<std::uint64_t> total_size;
  std::optional<std::uint64_t> offset;
  std::optional<std::uint64_t> credit;
  std::optional<Digest> digest;
  std::optional<bool> fin;
  std::optional<ErrorCode> error_code;
  std::optional<std::string> error_text;
  std::optional<std::string> token;
  std::optional<std::string> step_id;
  std::optional<std::string> name;
  std::optional<std::uint64_t> chunk_size;

  bool operator==(const Metadata&) const = default;
};

std::string encode(const Metadata& m);

enum class ParseError {
  BadHeader,     // wrong signature or frame size
  BadVersion,    // unsupported protocol version
  BadMetadata,   // undecodable metadata frame
};

std::string_view to_string(ParseError e) noexcept;

// Decodes a metadata frame. Unknown fields are ignored (forward compatibility).
std::expected<Metadata, ParseError> decode_metadata(std::string_view frame);

std::array<char, kHeaderSize> encode_header(Command c) noexcept;

// Validates signature and version and returns the command byte.
// An unknown command byte is returned as-is so the caller can answer with an error.
std::expected<Command, ParseError> decode_header(std::string_view frame) noexcept;

}  // namespace jrelay::proto
