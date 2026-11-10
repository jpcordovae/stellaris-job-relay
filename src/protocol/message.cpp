#include "protocol/message.hpp"

#include <algorithm>

#include "protocol/wire.hpp"

namespace jrelay::proto {

std::string_view to_string(Command c) noexcept {
  switch (c) {
    case Command::Hello: return "HELLO";
    case Command::Welcome: return "WELCOME";
    case Command::Put: return "PUT";
    case Command::Data: return "DATA";
    case Command::Commit: return "COMMIT";
    case Command::Get: return "GET";
    case Command::Credit: return "CREDIT";
    case Command::End: return "END";
    case Command::Release: return "RELEASE";
    case Command::Ok: return "OK";
    case Command::Error: return "ERROR";
  }
  return "UNKNOWN";
}

std::string_view to_string(ErrorCode c) noexcept {
  switch (c) {
    case ErrorCode::None: return "none";
    case ErrorCode::BadRequest: return "bad request";
    case ErrorCode::Unauthorized: return "unauthorized";
    case ErrorCode::NotFound: return "not found";
    case ErrorCode::NotReady: return "not ready";
    case ErrorCode::RunFinished: return "run finished";
    case ErrorCode::NoMemory: return "not enough memory on the relay";
    case ErrorCode::IntegrityMismatch: return "integrity check failed";
    case ErrorCode::TooLarge: return "too large";
    case ErrorCode::Busy: return "busy";
    case ErrorCode::Timeout: return "timeout";
    case ErrorCode::Internal: return "internal error";
  }
  return "unknown error";
}

std::string_view to_string(ParseError e) noexcept {
  switch (e) {
    case ParseError::BadHeader: return "bad header";
    case ParseError::BadVersion: return "unsupported protocol version";
    case ParseError::BadMetadata: return "bad metadata";
  }
  return "unknown parse error";
}

std::string encode(const Metadata& m) {
  wire::Writer w;
  if (m.job_id) w.bytes(field::kJobId, *m.job_id);
  if (m.total_size) w.varint(field::kTotalSize, *m.total_size);
  if (m.offset) w.varint(field::kOffset, *m.offset);
  if (m.credit) w.varint(field::kCredit, *m.credit);
  if (m.digest) {
    w.bytes(field::kDigest,
            std::string_view(reinterpret_cast<const char*>(m.digest->data()), m.digest->size()));
  }
  if (m.fin) w.varint(field::kFin, *m.fin ? 1 : 0);
  if (m.error_code) w.varint(field::kErrorCode, static_cast<std::uint64_t>(*m.error_code));
  if (m.error_text) w.bytes(field::kErrorText, *m.error_text);
  if (m.token) w.bytes(field::kToken, *m.token);
  if (m.step_id) w.bytes(field::kStepId, *m.step_id);
  if (m.name) w.bytes(field::kName, *m.name);
  if (m.chunk_size) w.varint(field::kChunkSize, *m.chunk_size);
  return std::move(w).take();
}

namespace {

// A known field number with the wrong wire type is treated as malformed rather
// than silently ignored: that would hide a real bug in a peer.
bool expect(const wire::Field& f, wire::WireType t) { return f.type == t; }

}  // namespace

std::expected<Metadata, ParseError> decode_metadata(std::string_view frame) {
  Metadata m;
  wire::Reader reader(frame);
  while (true) {
    auto next = reader.next();
    if (!next) return std::unexpected(ParseError::BadMetadata);
    if (!next->has_value()) break;
    const wire::Field& f = **next;

    using wire::WireType;
    switch (f.number) {
      case field::kJobId:
        if (!expect(f, WireType::Len)) return std::unexpected(ParseError::BadMetadata);
        m.job_id = std::string(f.bytes);
        break;
      case field::kTotalSize:
        if (!expect(f, WireType::Varint)) return std::unexpected(ParseError::BadMetadata);
        m.total_size = f.integer;
        break;
      case field::kOffset:
        if (!expect(f, WireType::Varint)) return std::unexpected(ParseError::BadMetadata);
        m.offset = f.integer;
        break;
      case field::kCredit:
        if (!expect(f, WireType::Varint)) return std::unexpected(ParseError::BadMetadata);
        m.credit = f.integer;
        break;
      case field::kDigest: {
        if (!expect(f, WireType::Len) || f.bytes.size() != kDigestSize) {
          return std::unexpected(ParseError::BadMetadata);
        }
        Digest d{};
        std::copy(f.bytes.begin(), f.bytes.end(), reinterpret_cast<char*>(d.data()));
        m.digest = d;
        break;
      }
      case field::kFin:
        if (!expect(f, WireType::Varint)) return std::unexpected(ParseError::BadMetadata);
        m.fin = f.integer != 0;
        break;
      case field::kErrorCode:
        if (!expect(f, WireType::Varint)) return std::unexpected(ParseError::BadMetadata);
        m.error_code = static_cast<ErrorCode>(static_cast<std::uint32_t>(f.integer));
        break;
      case field::kErrorText:
        if (!expect(f, WireType::Len)) return std::unexpected(ParseError::BadMetadata);
        m.error_text = std::string(f.bytes);
        break;
      case field::kToken:
        if (!expect(f, WireType::Len)) return std::unexpected(ParseError::BadMetadata);
        m.token = std::string(f.bytes);
        break;
      case field::kStepId:
        if (!expect(f, WireType::Len)) return std::unexpected(ParseError::BadMetadata);
        m.step_id = std::string(f.bytes);
        break;
      case field::kName:
        if (!expect(f, WireType::Len)) return std::unexpected(ParseError::BadMetadata);
        m.name = std::string(f.bytes);
        break;
      case field::kChunkSize:
        if (!expect(f, WireType::Varint)) return std::unexpected(ParseError::BadMetadata);
        m.chunk_size = f.integer;
        break;
      default:
        break;  // unknown field: ignore
    }
  }
  return m;
}

std::array<char, kHeaderSize> encode_header(Command c) noexcept {
  return {'J', 'R', static_cast<char>(kVersion), static_cast<char>(c)};
}

std::expected<Command, ParseError> decode_header(std::string_view frame) noexcept {
  if (frame.size() != kHeaderSize || frame[0] != 'J' || frame[1] != 'R') {
    return std::unexpected(ParseError::BadHeader);
  }
  if (static_cast<std::uint8_t>(frame[2]) != kVersion) return std::unexpected(ParseError::BadVersion);
  return static_cast<Command>(static_cast<std::uint8_t>(frame[3]));
}

}  // namespace jrelay::proto
