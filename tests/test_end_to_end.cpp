// End-to-end tests: a real server on a TCP port and real clients, as in a
// workflow where steps and jobs pass messages through the relay.
#include <doctest/doctest.h>

#include <array>
#include <random>
#include <sstream>
#include <thread>

#include "client/client.hpp"
#include "server/server.hpp"

using namespace jrelay;
using client::Client;
using client::ClientOptions;
using proto::ErrorCode;

namespace {

struct TestRelay {
  zmq::context_t context{1};
  server::Server server;
  std::thread thread;

  explicit TestRelay(server::ServerConfig config)
      : server(context, with_local_bind(std::move(config)), std::make_shared<server::DevAuthenticator>()),
        thread([this] { server.run(); }) {}

  ~TestRelay() { stop(); }

  void stop() {
    server.stop();
    if (thread.joinable()) thread.join();
  }

  static server::ServerConfig with_local_bind(server::ServerConfig c) {
    c.bind = "tcp://127.0.0.1:*";
    return c;
  }
};

server::ServerConfig small_config() {
  server::ServerConfig c;
  c.memory_limit = 64ull << 20;
  c.max_message_size = 64ull << 20;
  c.chunk_size = 256 << 10;  // small chunks: many credit round trips
  return c;
}

std::string random_bytes(std::size_t n, unsigned seed) {
  std::mt19937_64 rng(seed);
  std::string s(n, '\0');
  for (auto& ch : s) ch = static_cast<char>(rng());
  return s;
}

ClientOptions options(const TestRelay& r, const std::string& run, const std::string& job) {
  ClientOptions o;
  o.endpoint = r.server.endpoint();
  o.token = "dev:" + run;
  o.job_id = job;
  o.timeout = std::chrono::milliseconds(5000);
  return o;
}

std::expected<client::TransferResult, client::ClientError> put(zmq::context_t& ctx, const ClientOptions& o,
                                                             const std::string& step,
                                                             const std::string& name,
                                                             const std::string& payload, bool declare = true) {
  Client c(ctx, o);
  if (auto ok = c.connect(); !ok) return std::unexpected(ok.error());
  std::istringstream in(payload);
  return c.put(step, name, in, declare ? std::optional<std::uint64_t>(payload.size()) : std::nullopt);
}

std::expected<std::string, client::ClientError> get(zmq::context_t& ctx, const ClientOptions& o,
                                                   std::optional<std::string> job, const std::string& step,
                                                   const std::string& name) {
  Client c(ctx, o);
  if (auto ok = c.connect(); !ok) return std::unexpected(ok.error());
  std::ostringstream out;
  auto r = c.get(job, step, name, out);
  if (!r) return std::unexpected(r.error());
  return out.str();
}

}  // namespace

TEST_CASE("step to step in the same job, and job to job in the same run") {
  TestRelay relay(small_config());
  zmq::context_t ctx(1);
  const std::string payload = random_bytes((5 << 20) + 12345, 1);  // not a multiple of the chunk size

  auto up = put(ctx, options(relay, "100", "build"), "compile", "bin", payload);
  REQUIRE_MESSAGE(up, up.error().text);
  CHECK(up->size == payload.size());

  // A later step of the same job: job omitted.
  auto same_job = get(ctx, options(relay, "100", "build"), std::nullopt, "compile", "bin");
  REQUIRE_MESSAGE(same_job, same_job.error().text);
  CHECK(*same_job == payload);

  // Another job of the same run.
  auto other_job = get(ctx, options(relay, "100", "test"), "build", "compile", "bin");
  REQUIRE_MESSAGE(other_job, other_job.error().text);
  CHECK(*other_job == payload);
}

TEST_CASE("uploads without a declared size, empty messages and the default name") {
  TestRelay relay(small_config());
  zmq::context_t ctx(1);
  const std::string payload = random_bytes(3 << 20, 2);
  REQUIRE(put(ctx, options(relay, "7", "build"), "pack", "default", payload, /*declare=*/false));
  CHECK(get(ctx, options(relay, "7", "deploy"), "build", "pack", "default").value() == payload);

  REQUIRE(put(ctx, options(relay, "7", "build"), "flag", "empty", ""));
  CHECK(get(ctx, options(relay, "7", "build"), std::nullopt, "flag", "empty").value().empty());
}

TEST_CASE("several readers download the same message at the same time") {
  TestRelay relay(small_config());
  zmq::context_t ctx(1);
  const std::string payload = random_bytes(8 << 20, 3);
  REQUIRE(put(ctx, options(relay, "9", "build"), "compile", "bin", payload));

  std::array<std::string, 4> results;
  std::vector<std::thread> readers;
  for (std::size_t i = 0; i < results.size(); ++i) {
    readers.emplace_back([&, i] {
      auto r = get(ctx, options(relay, "9", "test" + std::to_string(i)), "build", "compile", "bin");
      if (r) results[i] = std::move(*r);
    });
  }
  for (auto& t : readers) t.join();
  for (const auto& r : results) CHECK(r == payload);
}

TEST_CASE("D1: the relay rejects what does not fit in memory, with a clear error") {
  auto config = small_config();
  config.memory_limit = 4ull << 20;
  config.max_message_size = 64ull << 20;
  TestRelay relay(config);
  zmq::context_t ctx(1);
  const std::string big = random_bytes(6 << 20, 4);

  auto declared = put(ctx, options(relay, "1", "build"), "s", "big", big, true);
  REQUIRE_FALSE(declared);
  CHECK(declared.error().code == ErrorCode::NoMemory);

  auto streamed = put(ctx, options(relay, "1", "build"), "s", "big", big, false);
  REQUIRE_FALSE(streamed);
  CHECK(streamed.error().code == ErrorCode::NoMemory);

  // The failed uploads gave their memory back: a message that fits still works.
  const std::string fits = random_bytes(3 << 20, 5);
  REQUIRE(put(ctx, options(relay, "1", "build"), "s", "small", fits));
  CHECK(get(ctx, options(relay, "1", "build"), std::nullopt, "s", "small").value() == fits);
}

TEST_CASE("isolation: other runs see nothing, and jobs only write their own messages") {
  TestRelay relay(small_config());
  zmq::context_t ctx(1);
  REQUIRE(put(ctx, options(relay, "1", "build"), "compile", "bin", "secret"));

  auto other_run = get(ctx, options(relay, "2", "build"), std::nullopt, "compile", "bin");
  REQUIRE_FALSE(other_run);
  CHECK(other_run.error().code == ErrorCode::NotFound);

  // A hand-made client: job "test" says HELLO, then tries to PUT under "build".
  zmq::socket_t raw(ctx, zmq::socket_type::dealer);
  raw.set(zmq::sockopt::linger, 0);
  raw.connect(relay.server.endpoint());
  auto exchange = [&](proto::Command cmd, const proto::Metadata& m) {
    const auto h = proto::encode_header(cmd);
    raw.send(zmq::buffer(h), zmq::send_flags::sndmore);
    raw.send(zmq::buffer(proto::encode(m)), zmq::send_flags::none);
    std::vector<zmq::message_t> parts;
    REQUIRE(zmq::recv_multipart(raw, std::back_inserter(parts)));
    REQUIRE(parts.size() >= 2);
    auto c = proto::decode_header({static_cast<const char*>(parts[0].data()), parts[0].size()});
    auto md = proto::decode_metadata({static_cast<const char*>(parts[1].data()), parts[1].size()});
    REQUIRE(c);
    REQUIRE(md);
    return std::pair{*c, *md};
  };
  proto::Metadata hello;
  hello.token = "dev:1";
  hello.job_id = "test";
  CHECK(exchange(proto::Command::Hello, hello).first == proto::Command::Welcome);

  proto::Metadata forged;
  forged.job_id = "build";
  forged.step_id = "compile";
  forged.name = "bin";
  forged.total_size = 4;
  auto [cmd, meta] = exchange(proto::Command::Put, forged);
  CHECK(cmd == proto::Command::Error);
  CHECK(meta.error_code == ErrorCode::Unauthorized);

  // The original message is untouched.
  CHECK(get(ctx, options(relay, "1", "test"), "build", "compile", "bin").value() == "secret");
}

TEST_CASE("malformed frames get an error, not a crash") {
  TestRelay relay(small_config());
  zmq::context_t ctx(1);
  zmq::socket_t raw(ctx, zmq::socket_type::dealer);
  raw.set(zmq::sockopt::linger, 0);
  raw.connect(relay.server.endpoint());

  auto send_and_expect_error = [&](std::string_view header, std::string_view meta) {
    raw.send(zmq::buffer(header), zmq::send_flags::sndmore);
    raw.send(zmq::buffer(meta), zmq::send_flags::none);
    std::vector<zmq::message_t> parts;
    REQUIRE(zmq::recv_multipart(raw, std::back_inserter(parts)));
    auto c = proto::decode_header({static_cast<const char*>(parts[0].data()), parts[0].size()});
    REQUIRE(c);
    CHECK(*c == proto::Command::Error);
  };
  send_and_expect_error("XXXX", "");                                   // bad signature
  send_and_expect_error(std::string_view("JR\x09\x01", 4), "");        // bad version
  send_and_expect_error(std::string_view("JR\x01\x01", 4), "\x0A\x7F");  // truncated metadata
  send_and_expect_error(std::string_view("JR\x01\x10", 4), "");        // PUT before HELLO
  send_and_expect_error(std::string_view("JR\x01\x55", 4), "");        // unknown command (no HELLO yet)

  // The relay is still healthy.
  REQUIRE(put(ctx, options(relay, "1", "build"), "s", "n", "ok"));
}

TEST_CASE("after the run is released, data is gone and the error says why") {
  TestRelay relay(small_config());
  zmq::context_t ctx(1);
  REQUIRE(put(ctx, options(relay, "5", "build"), "compile", "bin", random_bytes(1 << 20, 6)));

  Client c(ctx, options(relay, "5", "cleanup"));
  REQUIRE(c.connect());
  auto released = c.release();
  REQUIRE(released);
  CHECK(*released == 1);

  auto late = get(ctx, options(relay, "5", "test"), "build", "compile", "bin");
  REQUIRE_FALSE(late);
  CHECK(late.error().code == ErrorCode::RunFinished);

  relay.stop();
  CHECK(relay.server.stats().memory_used == 0);
}

TEST_CASE("unknown messages and bad tokens") {
  TestRelay relay(small_config());
  zmq::context_t ctx(1);
  auto none = get(ctx, options(relay, "3", "test"), "build", "nope", "x");
  REQUIRE_FALSE(none);
  CHECK(none.error().code == ErrorCode::NotFound);

  auto o = options(relay, "3", "test");
  o.token = "not-a-dev-token";
  Client c(ctx, o);
  auto hello = c.connect();
  REQUIRE_FALSE(hello);
  CHECK(hello.error().code == ErrorCode::Unauthorized);
}

TEST_CASE("CURVE: encrypted transfer works, and clients without the key get nothing") {
  std::array<char, 41> pub{};
  std::array<char, 41> sec{};
  REQUIRE(zmq_curve_keypair(pub.data(), sec.data()) == 0);
  auto config = small_config();
  config.curve_secret_key = std::string(sec.data(), 40);
  TestRelay relay(config);
  zmq::context_t ctx(1);

  auto secure = options(relay, "11", "build");
  secure.server_public_key = std::string(pub.data(), 40);
  const std::string payload = random_bytes(2 << 20, 7);
  REQUIRE(put(ctx, secure, "compile", "bin", payload));
  CHECK(get(ctx, secure, std::nullopt, "compile", "bin").value() == payload);

  auto plain = options(relay, "11", "build");
  plain.timeout = std::chrono::milliseconds(500);
  Client c(ctx, plain);
  auto hello = c.connect();
  REQUIRE_FALSE(hello);
  CHECK(hello.error().code == ErrorCode::Timeout);
}
