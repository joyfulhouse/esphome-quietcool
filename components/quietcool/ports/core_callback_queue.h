#pragma once

#include "quietcool/core/core_types.h"
#include "quietcool/core/ring_buffer.h"

#include <array>
#include <optional>

namespace quietcool {

enum class CoreCallbackKind : std::uint8_t {
  TxRejected,
  RadioRecovered,
};

struct CoreCallback final {
  CoreCallbackKind kind;
  std::optional<TxToken> token;
  MonotonicMs now_ms;
};

class CoreCallbackQueue final {
 public:
  static constexpr std::size_t kCapacity = 8;

  bool enqueue(CoreCallbackKind kind, std::optional<TxToken> token) {
    return ring_.push(StoredCoreCallback{kind, token});
  }

  // Timestamped at pop rather than at enqueue: a deferred callback is applied
  // in the loop iteration that drains it, and the core must see that
  // iteration's clock, not the one that queued the work.
  std::optional<CoreCallback> pop(MonotonicMs now_ms) {
    const auto stored = ring_.pop();
    if (!stored) return std::nullopt;
    return CoreCallback{stored->kind, stored->token, now_ms};
  }

  std::size_t size() const { return ring_.size(); }

 private:
  struct StoredCoreCallback final {
    CoreCallbackKind kind;
    std::optional<TxToken> token;
  };

  RingBuffer<StoredCoreCallback, kCapacity> ring_;
};

}  // namespace quietcool
