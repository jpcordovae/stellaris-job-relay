#include "server/memory.hpp"

#include <utility>

namespace jrelay::server {

std::optional<Reservation> MemoryBudget::try_reserve(std::uint64_t bytes) noexcept {
  if (!try_take(bytes)) return std::nullopt;
  return Reservation(this, bytes);
}

bool MemoryBudget::try_take(std::uint64_t bytes) noexcept {
  if (bytes > limit_ - used_) return false;
  used_ += bytes;
  return true;
}

void MemoryBudget::give_back(std::uint64_t bytes) noexcept { used_ -= bytes; }

Reservation::Reservation(Reservation&& other) noexcept
    : budget_(std::exchange(other.budget_, nullptr)), size_(std::exchange(other.size_, 0)) {}

Reservation& Reservation::operator=(Reservation&& other) noexcept {
  if (this != &other) {
    reset();
    budget_ = std::exchange(other.budget_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

Reservation::~Reservation() { reset(); }

bool Reservation::grow(std::uint64_t bytes) noexcept {
  if (budget_ == nullptr || !budget_->try_take(bytes)) return false;
  size_ += bytes;
  return true;
}

void Reservation::reset() noexcept {
  if (budget_ != nullptr) budget_->give_back(size_);
  budget_ = nullptr;
  size_ = 0;
}

}  // namespace jrelay::server
