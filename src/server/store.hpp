// In-memory message store, indexed by run and by (job, step, name).
//
// Ownership (see docs/protocolo.md §7):
//   - an upload in progress has one owner: std::unique_ptr<BlobBuilder>;
//   - on COMMIT it becomes std::shared_ptr<const Blob>, shared by the index and
//     by every download in progress. Releasing a run never frees memory that a
//     download is still sending;
//   - payload frames are moved (std::move), never copied.
#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <zmq.hpp>

#include "common/digest.hpp"
#include "common/ids.hpp"
#include "protocol/message.hpp"
#include "server/memory.hpp"

namespace jrelay::server {

using Clock = std::chrono::steady_clock;
using proto::ErrorCode;

// Who wrote a message, for auditing (decision P3).
struct Producer {
  std::string check_run_id;
  std::string run_attempt;
};

// An immutable, committed message.
class Blob {
 public:
  Blob(std::vector<zmq::message_t> chunks, std::uint64_t size, proto::Digest digest,
       Reservation reservation, Producer producer);

  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  [[nodiscard]] const proto::Digest& digest() const noexcept { return digest_; }
  [[nodiscard]] const Producer& producer() const noexcept { return producer_; }
  [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }
  [[nodiscard]] std::uint64_t chunk_offset(std::size_t i) const noexcept { return offsets_[i]; }
  [[nodiscard]] std::size_t chunk_size(std::size_t i) const noexcept { return chunks_[i].size(); }

  // Index of the chunk that starts exactly at `offset`, if any (used to resume).
  [[nodiscard]] std::optional<std::size_t> chunk_at(std::uint64_t offset) const noexcept;

  // Returns a message that shares the chunk's content without copying it
  // (zmq_msg_copy uses reference counting for large messages).
  [[nodiscard]] zmq::message_t share_chunk(std::size_t i) const;

 private:
  // mutable: zmq_msg_copy() updates the source's reference-count bookkeeping.
  // The payload bytes themselves are never modified after commit.
  mutable std::vector<zmq::message_t> chunks_;
  std::vector<std::uint64_t> offsets_;
  std::uint64_t size_;
  proto::Digest digest_;
  Reservation reservation_;
  Producer producer_;
};

// A message being uploaded. Single owner until COMMIT.
class BlobBuilder {
 public:
  BlobBuilder(std::string run_id, Key key, std::optional<std::uint64_t> declared_size,
              Reservation reservation, Producer producer, std::uint64_t max_size);

  // Appends one DATA frame. `offset` must equal the bytes received so far.
  [[nodiscard]] std::expected<void, ErrorCode> append(std::uint64_t offset, zmq::message_t&& data);

  [[nodiscard]] const std::string& run_id() const noexcept { return run_id_; }
  [[nodiscard]] const Key& key() const noexcept { return key_; }
  [[nodiscard]] std::uint64_t received() const noexcept { return received_; }

  // Verifies size and digest and turns the builder into an immutable Blob.
  [[nodiscard]] static std::expected<std::shared_ptr<const Blob>, ErrorCode> finish(
      std::unique_ptr<BlobBuilder> self, const proto::Digest& client_digest);

 private:
  std::string run_id_;
  Key key_;
  std::optional<std::uint64_t> declared_size_;
  Reservation reservation_;
  Producer producer_;
  std::uint64_t max_size_;
  std::vector<zmq::message_t> chunks_;
  std::uint64_t received_ = 0;
  Hasher hasher_;
};

class Store {
 public:
  Store(MemoryBudget& budget, std::uint64_t max_message_size) noexcept
      : budget_(budget), max_message_size_(max_message_size) {}

  // Starts an upload. Reserves the declared size up front (D1); when the size is
  // unknown, memory is reserved as data arrives and the upload fails as soon as
  // it no longer fits.
  [[nodiscard]] std::expected<std::unique_ptr<BlobBuilder>, ErrorCode> begin_upload(
      const std::string& run_id, const Key& key, std::optional<std::uint64_t> declared_size,
      Producer producer, Clock::time_point now);

  // Publishes a finished upload. A re-run of the same step replaces the
  // previous message.
  [[nodiscard]] std::expected<std::shared_ptr<const Blob>, ErrorCode> commit(
      std::unique_ptr<BlobBuilder> upload, const proto::Digest& client_digest);

  // Drops an upload that will not be committed (error or disconnect).
  void abort_upload(std::unique_ptr<BlobBuilder> upload) noexcept;

  [[nodiscard]] std::expected<std::shared_ptr<const Blob>, ErrorCode> find(
      const std::string& run_id, const Key& key) const;

  // Frees every message of the run. Later requests for it get RunFinished.
  std::size_t release_run(const std::string& run_id, Clock::time_point now);

  // Safety net: releases runs older than `max_run_age` and forgets released
  // runs after the same period.
  std::size_t sweep(Clock::time_point now, std::chrono::seconds max_run_age);

  [[nodiscard]] std::size_t run_count() const noexcept { return runs_.size(); }
  [[nodiscard]] std::size_t message_count() const noexcept;

 private:
  struct Run {
    Clock::time_point created;
    std::map<Key, std::shared_ptr<const Blob>> messages;
  };

  MemoryBudget& budget_;
  std::uint64_t max_message_size_;
  std::map<std::string, Run> runs_;
  std::map<std::string, Clock::time_point> released_;
  std::set<std::pair<std::string, Key>> uploading_;
};

}  // namespace jrelay::server
