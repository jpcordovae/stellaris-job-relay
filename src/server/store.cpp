#include "server/store.hpp"

#include <algorithm>
#include <utility>

namespace jrelay::server {

// ---------------------------------------------------------------- Blob

Blob::Blob(std::vector<zmq::message_t> chunks, std::uint64_t size, proto::Digest digest,
           Reservation reservation, Producer producer)
    : chunks_(std::move(chunks)),
      size_(size),
      digest_(digest),
      reservation_(std::move(reservation)),
      producer_(std::move(producer)) {
  offsets_.reserve(chunks_.size());
  std::uint64_t offset = 0;
  for (const auto& c : chunks_) {
    offsets_.push_back(offset);
    offset += c.size();
  }
}

std::optional<std::size_t> Blob::chunk_at(std::uint64_t offset) const noexcept {
  if (offset == size_) return chunks_.size();  // resume at the very end: nothing left
  auto it = std::ranges::lower_bound(offsets_, offset);
  if (it == offsets_.end() || *it != offset) return std::nullopt;
  return static_cast<std::size_t>(it - offsets_.begin());
}

zmq::message_t Blob::share_chunk(std::size_t i) const {
  zmq::message_t out;
  out.copy(chunks_[i]);
  return out;
}

// ---------------------------------------------------------------- BlobBuilder

BlobBuilder::BlobBuilder(std::string run_id, Key key, std::optional<std::uint64_t> declared_size,
                         Reservation reservation, Producer producer, std::uint64_t max_size)
    : run_id_(std::move(run_id)),
      key_(std::move(key)),
      declared_size_(declared_size),
      reservation_(std::move(reservation)),
      producer_(std::move(producer)),
      max_size_(max_size) {}

std::expected<void, ErrorCode> BlobBuilder::append(std::uint64_t offset, zmq::message_t&& data) {
  if (offset != received_) return std::unexpected(ErrorCode::BadRequest);
  const std::uint64_t n = data.size();
  if (n > max_size_ - received_) return std::unexpected(ErrorCode::TooLarge);
  if (declared_size_) {
    if (n > *declared_size_ - received_) return std::unexpected(ErrorCode::TooLarge);
  } else if (!reservation_.grow(n)) {
    return std::unexpected(ErrorCode::NoMemory);
  }
  hasher_.update(data.data(), data.size());
  received_ += n;
  if (n > 0) chunks_.push_back(std::move(data));
  return {};
}

std::expected<std::shared_ptr<const Blob>, ErrorCode> BlobBuilder::finish(
    std::unique_ptr<BlobBuilder> self, const proto::Digest& client_digest) {
  if (self->declared_size_ && self->received_ != *self->declared_size_) {
    return std::unexpected(ErrorCode::BadRequest);
  }
  const proto::Digest digest = self->hasher_.finish();
  if (digest != client_digest) return std::unexpected(ErrorCode::IntegrityMismatch);
  return std::make_shared<const Blob>(std::move(self->chunks_), self->received_, digest,
                                      std::move(self->reservation_), std::move(self->producer_));
}

// ---------------------------------------------------------------- Store

std::expected<std::unique_ptr<BlobBuilder>, ErrorCode> Store::begin_upload(
    const std::string& run_id, const Key& key, std::optional<std::uint64_t> declared_size,
    Producer producer, Clock::time_point now) {
  if (released_.contains(run_id)) return std::unexpected(ErrorCode::RunFinished);
  if (declared_size && *declared_size > max_message_size_) return std::unexpected(ErrorCode::TooLarge);
  if (uploading_.contains({run_id, key})) return std::unexpected(ErrorCode::Busy);

  auto reservation = budget_.try_reserve(declared_size.value_or(0));
  if (!reservation) return std::unexpected(ErrorCode::NoMemory);

  runs_.try_emplace(run_id, Run{now, {}});
  uploading_.insert({run_id, key});
  return std::make_unique<BlobBuilder>(run_id, key, declared_size, std::move(*reservation),
                                       std::move(producer), max_message_size_);
}

std::expected<std::shared_ptr<const Blob>, ErrorCode> Store::commit(
    std::unique_ptr<BlobBuilder> upload, const proto::Digest& client_digest) {
  const std::string run_id = upload->run_id();
  const Key key = upload->key();
  uploading_.erase({run_id, key});

  auto run = runs_.find(run_id);
  if (run == runs_.end()) return std::unexpected(ErrorCode::RunFinished);  // released meanwhile

  auto blob = BlobBuilder::finish(std::move(upload), client_digest);
  if (!blob) return std::unexpected(blob.error());
  run->second.messages.insert_or_assign(key, *blob);
  return blob;
}

void Store::abort_upload(std::unique_ptr<BlobBuilder> upload) noexcept {
  if (upload) uploading_.erase({upload->run_id(), upload->key()});
}

std::expected<std::shared_ptr<const Blob>, ErrorCode> Store::find(const std::string& run_id,
                                                                 const Key& key) const {
  if (released_.contains(run_id)) return std::unexpected(ErrorCode::RunFinished);
  if (uploading_.contains({run_id, key})) {
    // A re-run may be replacing an existing message; serve neither half until COMMIT.
    return std::unexpected(ErrorCode::NotReady);
  }
  auto run = runs_.find(run_id);
  if (run == runs_.end()) return std::unexpected(ErrorCode::NotFound);
  auto msg = run->second.messages.find(key);
  if (msg == run->second.messages.end()) return std::unexpected(ErrorCode::NotFound);
  return msg->second;
}

std::size_t Store::release_run(const std::string& run_id, Clock::time_point now) {
  std::size_t freed = 0;
  if (auto run = runs_.find(run_id); run != runs_.end()) {
    freed = run->second.messages.size();
    runs_.erase(run);
  }
  released_.insert_or_assign(run_id, now);
  return freed;
}

std::size_t Store::sweep(Clock::time_point now, std::chrono::seconds max_run_age) {
  std::vector<std::string> expired;
  for (const auto& [id, run] : runs_) {
    if (now - run.created > max_run_age) expired.push_back(id);
  }
  for (const auto& id : expired) release_run(id, now);
  std::erase_if(released_, [&](const auto& kv) { return now - kv.second > max_run_age; });
  return expired.size();
}

std::size_t Store::message_count() const noexcept {
  std::size_t n = 0;
  for (const auto& [id, run] : runs_) n += run.messages.size();
  return n;
}

}  // namespace jrelay::server
