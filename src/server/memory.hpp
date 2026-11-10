// Memory budget for message payloads (decision D1: reject what does not fit).
//
// The budget counts payload bytes only. It is a hard limit on what the relay
// accepts, checked before data arrives whenever the size is declared.
//
// The server runs a single event loop thread, so the budget is not
// thread-safe on purpose.
#pragma once

#include <cstdint>
#include <optional>

namespace jrelay::server {

class Reservation;

class MemoryBudget {
 public:
  explicit MemoryBudget(std::uint64_t limit_bytes) noexcept : limit_(limit_bytes) {}
  MemoryBudget(const MemoryBudget&) = delete;
  MemoryBudget& operator=(const MemoryBudget&) = delete;

  // Reserves `bytes` or returns std::nullopt if they do not fit.
  [[nodiscard]] std::optional<Reservation> try_reserve(std::uint64_t bytes) noexcept;

  [[nodiscard]] std::uint64_t used() const noexcept { return used_; }
  [[nodiscard]] std::uint64_t limit() const noexcept { return limit_; }
  [[nodiscard]] std::uint64_t available() const noexcept { return limit_ - used_; }

 private:
  friend class Reservation;
  bool try_take(std::uint64_t bytes) noexcept;
  void give_back(std::uint64_t bytes) noexcept;

  std::uint64_t limit_;
  std::uint64_t used_ = 0;
};

// Move-only handle to reserved bytes. Returns them to the budget on destruction,
// so a message's memory is accounted for exactly as long as the message exists.
class Reservation {
 public:
  Reservation() noexcept = default;
  Reservation(Reservation&& other) noexcept;
  Reservation& operator=(Reservation&& other) noexcept;
  Reservation(const Reservation&) = delete;
  Reservation& operator=(const Reservation&) = delete;
  ~Reservation();

  // Reserves `bytes` more. Returns false (and changes nothing) if they do not fit.
  [[nodiscard]] bool grow(std::uint64_t bytes) noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }

 private:
  friend class MemoryBudget;
  Reservation(MemoryBudget* budget, std::uint64_t size) noexcept : budget_(budget), size_(size) {}
  void reset() noexcept;

  MemoryBudget* budget_ = nullptr;
  std::uint64_t size_ = 0;
};

}  // namespace jrelay::server
