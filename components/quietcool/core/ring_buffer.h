#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

namespace quietcool {

// A bounded FIFO with fail-closed admission.
//
// This exists because the effect queue and the core-callback queue were two
// separate hand-written implementations of the same ring: the same
// head_/tail_/size_ triple and the same (index + 1) % kCapacity arithmetic,
// written independently under crash pressure. Two copies of the same
// off-by-one-prone index math drift the moment a boundary fix lands on only
// one of them, and both sit on the path that already caused kernel corruption
// once.
//
// Admission is fail-closed by design: enqueue() returns false rather than
// overwriting the oldest entry. Dropping the newest is recoverable and
// observable; silently discarding an already-accepted effect (a RevokeTxLease,
// say) would strand a transmit lease.
template <typename T, std::size_t Capacity>
class RingBuffer final {
 public:
  static_assert(Capacity > 0, "ring buffer capacity must be non-zero");

  static constexpr std::size_t kCapacity = Capacity;

  std::size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }
  bool full() const { return size_ == kCapacity; }
  std::size_t available() const { return kCapacity - size_; }

  bool push(T value) {
    if (full()) return false;
    slots_[tail_] = std::move(value);
    tail_ = (tail_ + 1U) % kCapacity;
    ++size_;
    return true;
  }

  std::optional<T> pop() {
    if (empty()) return std::nullopt;
    auto value = std::move(slots_[head_]);
    slots_[head_].reset();
    head_ = (head_ + 1U) % kCapacity;
    --size_;
    return value;
  }

 private:
  std::array<std::optional<T>, kCapacity> slots_{};
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t size_{0};
};

}  // namespace quietcool
