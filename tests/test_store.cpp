#include <doctest/doctest.h>

#include <string>

#include "common/digest.hpp"
#include "server/memory.hpp"
#include "server/store.hpp"

using namespace jrelay;
using namespace jrelay::server;
using proto::ErrorCode;

namespace {

zmq::message_t msg(const std::string& s) { return zmq::message_t(s.data(), s.size()); }

proto::Digest digest_of(const std::string& s) {
  Hasher h;
  h.update(s.data(), s.size());
  return h.finish();
}

const auto t0 = Clock::now();
const Key kKey{"build", "compile", "bin"};

// Uploads `chunks` under (run, key) and commits.
std::expected<std::shared_ptr<const Blob>, ErrorCode> upload(Store& store, const std::string& run,
                                                            const Key& key,
                                                            const std::vector<std::string>& chunks,
                                                            bool declare = true) {
  std::string all;
  for (const auto& c : chunks) all += c;
  auto b = store.begin_upload(run, key, declare ? std::optional<std::uint64_t>(all.size()) : std::nullopt,
                              {}, t0);
  if (!b) return std::unexpected(b.error());
  std::uint64_t off = 0;
  for (const auto& c : chunks) {
    if (auto ok = (*b)->append(off, msg(c)); !ok) {
      store.abort_upload(std::move(*b));
      return std::unexpected(ok.error());
    }
    off += c.size();
  }
  return store.commit(std::move(*b), digest_of(all));
}

std::string read_all(const Blob& blob) {
  std::string s;
  for (std::size_t i = 0; i < blob.chunk_count(); ++i) {
    auto m = blob.share_chunk(i);
    s.append(static_cast<const char*>(m.data()), m.size());
  }
  return s;
}

}  // namespace

TEST_CASE("memory budget reserves, rejects and returns bytes") {
  MemoryBudget budget(100);
  {
    auto a = budget.try_reserve(60);
    REQUIRE(a);
    CHECK(budget.used() == 60);
    CHECK_FALSE(budget.try_reserve(41));
    CHECK(a->grow(40));
    CHECK_FALSE(a->grow(1));
    CHECK(budget.used() == 100);
    Reservation moved = std::move(*a);
    CHECK(budget.used() == 100);
  }
  CHECK(budget.used() == 0);
}

TEST_CASE("upload, commit and read back a message") {
  MemoryBudget budget(1 << 20);
  Store store(budget, 1 << 20);
  auto blob = upload(store, "1", kKey, {"hello ", "relay"});
  REQUIRE(blob);
  CHECK((*blob)->size() == 11);
  auto found = store.find("1", kKey);
  REQUIRE(found);
  CHECK(read_all(**found) == "hello relay");
  CHECK(budget.used() == 11);
}

TEST_CASE("D1: a declared size that does not fit is rejected before any data") {
  MemoryBudget budget(10);
  Store store(budget, 1 << 20);
  auto b = store.begin_upload("1", kKey, 11, {}, t0);
  REQUIRE_FALSE(b);
  CHECK(b.error() == ErrorCode::NoMemory);
  CHECK(budget.used() == 0);
}

TEST_CASE("D1: an undeclared upload fails as soon as it stops fitting, and frees what it held") {
  MemoryBudget budget(10);
  Store store(budget, 1 << 20);
  auto r = upload(store, "1", kKey, {"123456", "789012"}, /*declare=*/false);
  REQUIRE_FALSE(r);
  CHECK(r.error() == ErrorCode::NoMemory);
  CHECK(budget.used() == 0);
  CHECK(store.find("1", kKey).error() == ErrorCode::NotFound);
}

TEST_CASE("messages above the maximum size are rejected") {
  MemoryBudget budget(1 << 20);
  Store store(budget, 8);
  CHECK(store.begin_upload("1", kKey, 9, {}, t0).error() == ErrorCode::TooLarge);
  CHECK(upload(store, "1", kKey, {"12345", "6789"}, false).error() == ErrorCode::TooLarge);
}

TEST_CASE("sending more data than declared is rejected") {
  MemoryBudget budget(1 << 20);
  Store store(budget, 1 << 20);
  auto b = store.begin_upload("1", kKey, 3, {}, t0);
  REQUIRE(b);
  CHECK((*b)->append(0, msg("abcd")).error() == ErrorCode::TooLarge);
}

TEST_CASE("a wrong digest is rejected and nothing is published") {
  MemoryBudget budget(1 << 20);
  Store store(budget, 1 << 20);
  auto b = store.begin_upload("1", kKey, 3, {}, t0);
  REQUIRE(b);
  REQUIRE((*b)->append(0, msg("abc")));
  auto c = store.commit(std::move(*b), digest_of("abd"));
  CHECK(c.error() == ErrorCode::IntegrityMismatch);
  CHECK(store.find("1", kKey).error() == ErrorCode::NotFound);
  CHECK(budget.used() == 0);
}

TEST_CASE("out-of-order data is rejected") {
  MemoryBudget budget(1 << 20);
  Store store(budget, 1 << 20);
  auto b = store.begin_upload("1", kKey, std::nullopt, {}, t0);
  REQUIRE(b);
  CHECK((*b)->append(5, msg("x")).error() == ErrorCode::BadRequest);
}

TEST_CASE("NotReady while uploading, Busy for a second upload of the same key") {
  MemoryBudget budget(1 << 20);
  Store store(budget, 1 << 20);
  auto b = store.begin_upload("1", kKey, std::nullopt, {}, t0);
  REQUIRE(b);
  CHECK(store.find("1", kKey).error() == ErrorCode::NotReady);
  CHECK(store.begin_upload("1", kKey, std::nullopt, {}, t0).error() == ErrorCode::Busy);
  store.abort_upload(std::move(*b));
  CHECK(store.find("1", kKey).error() == ErrorCode::NotFound);
}

TEST_CASE("a re-run replaces the previous message and frees its memory") {
  MemoryBudget budget(1 << 20);
  Store store(budget, 1 << 20);
  REQUIRE(upload(store, "1", kKey, {"first version"}));
  REQUIRE(upload(store, "1", kKey, {"second"}));
  CHECK(read_all(**store.find("1", kKey)) == "second");
  CHECK(budget.used() == 6);
}

TEST_CASE("runs are isolated from each other") {
  MemoryBudget budget(1 << 20);
  Store store(budget, 1 << 20);
  REQUIRE(upload(store, "1", kKey, {"run one"}));
  CHECK(store.find("2", kKey).error() == ErrorCode::NotFound);
}

TEST_CASE("releasing a run frees memory, but never under an active reader") {
  MemoryBudget budget(1 << 20);
  Store store(budget, 1 << 20);
  REQUIRE(upload(store, "1", kKey, {"payload"}));
  auto reader = store.find("1", kKey);  // a download in progress holds the blob
  REQUIRE(reader);

  CHECK(store.release_run("1", t0) == 1);
  CHECK(store.find("1", kKey).error() == ErrorCode::RunFinished);
  CHECK(store.begin_upload("1", kKey, 1, {}, t0).error() == ErrorCode::RunFinished);
  CHECK(budget.used() == 7);                 // still alive for the reader
  CHECK(read_all(**reader) == "payload");    // and still readable
  reader->reset();
  CHECK(budget.used() == 0);                 // freed when the last reader is done
}

TEST_CASE("sweep releases runs past the maximum age") {
  MemoryBudget budget(1 << 20);
  Store store(budget, 1 << 20);
  REQUIRE(upload(store, "1", kKey, {"old"}));
  CHECK(store.sweep(t0 + std::chrono::hours(1), std::chrono::hours(2)) == 0);
  CHECK(store.sweep(t0 + std::chrono::hours(3), std::chrono::hours(2)) == 1);
  CHECK(budget.used() == 0);
  CHECK(store.find("1", kKey).error() == ErrorCode::RunFinished);
}

TEST_CASE("resume offsets must fall on chunk boundaries") {
  MemoryBudget budget(1 << 20);
  Store store(budget, 1 << 20);
  auto blob = upload(store, "1", kKey, {"aaaa", "bbbb", "cc"});
  REQUIRE(blob);
  CHECK((*blob)->chunk_at(0) == 0u);
  CHECK((*blob)->chunk_at(8) == 2u);
  CHECK((*blob)->chunk_at(10) == 3u);  // the end: nothing left to send
  CHECK_FALSE((*blob)->chunk_at(5));
}
