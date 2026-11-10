#include <doctest/doctest.h>

#include <limits>

#include "common/ids.hpp"
#include "protocol/message.hpp"
#include "protocol/wire.hpp"

using namespace jrelay;

TEST_CASE("varint matches the protobuf examples and round-trips") {
  std::string out;
  wire::put_varint(out, 150);
  CHECK(out == std::string("\x96\x01", 2));  // the example in the protobuf encoding guide

  const std::uint64_t values[] = {0, 1, 127, 128, 300, 1ull << 32, 1ull << 63,
                                  std::numeric_limits<std::uint64_t>::max()};
  for (std::uint64_t v : values) {
    std::string buf;
    wire::put_varint(buf, v);
    CHECK(buf.size() <= wire::kMaxVarintBytes);
    std::string_view in = buf;
    auto got = wire::get_varint(in);
    REQUIRE(got);
    CHECK(*got == v);
    CHECK(in.empty());
  }
}

TEST_CASE("varint rejects truncated and overflowing input") {
  std::string_view truncated("\x80\x80", 2);
  CHECK(wire::get_varint(truncated).error() == wire::DecodeError::Truncated);

  std::string eleven(10, '\xFF');
  eleven.push_back('\x01');
  std::string_view in = eleven;
  CHECK(wire::get_varint(in).error() == wire::DecodeError::VarintOverflow);

  std::string tenth_too_big(9, '\xFF');
  tenth_too_big.push_back('\x02');
  std::string_view in2 = tenth_too_big;
  CHECK(wire::get_varint(in2).error() == wire::DecodeError::VarintOverflow);
}

TEST_CASE("metadata round-trips every field") {
  proto::Metadata m;
  m.job_id = "build";
  m.total_size = 5'000'000'000ull;
  m.offset = 4'194'304;
  m.credit = 16'777'216;
  proto::Digest d{};
  for (std::size_t i = 0; i < d.size(); ++i) d[i] = static_cast<std::uint8_t>(i * 7);
  m.digest = d;
  m.fin = true;
  m.error_code = proto::ErrorCode::NoMemory;
  m.error_text = "no cabe";
  m.token = "dev:123";
  m.step_id = "compile";
  m.name = "app.tar";
  m.chunk_size = 4 << 20;

  auto decoded = proto::decode_metadata(proto::encode(m));
  REQUIRE(decoded);
  CHECK(*decoded == m);
}

TEST_CASE("unknown fields are skipped for forward compatibility") {
  wire::Writer w;
  w.bytes(proto::field::kStepId, "compile");
  w.varint(19, 42);       // reserved number, unknown to this version
  w.bytes(500, "future"); // far-away unknown field
  w.varint(proto::field::kOffset, 7);
  auto m = proto::decode_metadata(w.data());
  REQUIRE(m);
  CHECK(m->step_id == "compile");
  CHECK(m->offset == 7u);
}

TEST_CASE("malformed metadata is rejected") {
  wire::Writer w;
  w.varint(proto::field::kStepId, 3);  // known field, wrong wire type
  CHECK_FALSE(proto::decode_metadata(w.data()));

  wire::Writer bad_digest;
  bad_digest.bytes(proto::field::kDigest, "short");
  CHECK_FALSE(proto::decode_metadata(bad_digest.data()));

  CHECK_FALSE(proto::decode_metadata(std::string_view("\x0A\x05" "ab", 4)));  // LEN past the end
  CHECK_FALSE(proto::decode_metadata(std::string_view("\x0B", 1)));           // wire type 3
  CHECK_FALSE(proto::decode_metadata(std::string_view("\x00\x00", 2)));       // field number 0
}

TEST_CASE("header validates signature and version") {
  auto h = proto::encode_header(proto::Command::Put);
  auto c = proto::decode_header(std::string_view(h.data(), h.size()));
  REQUIRE(c);
  CHECK(*c == proto::Command::Put);

  CHECK(proto::decode_header("JR").error() == proto::ParseError::BadHeader);
  CHECK(proto::decode_header(std::string_view("XR\x01\x10", 4)).error() == proto::ParseError::BadHeader);
  CHECK(proto::decode_header(std::string_view("JR\x02\x10", 4)).error() == proto::ParseError::BadVersion);
}

TEST_CASE("identifier validation") {
  CHECK(is_valid_job_or_step_id("build-linux_x64"));
  CHECK_FALSE(is_valid_job_or_step_id(""));
  CHECK_FALSE(is_valid_job_or_step_id("a/b"));
  CHECK_FALSE(is_valid_job_or_step_id(std::string(256, 'a')));
  CHECK(is_valid_name("app.tar.gz"));
  CHECK_FALSE(is_valid_name(".."));
  CHECK_FALSE(is_valid_name("a/b"));
  CHECK(is_valid_run_id("1234567890"));
  CHECK_FALSE(is_valid_run_id("12a"));
}
