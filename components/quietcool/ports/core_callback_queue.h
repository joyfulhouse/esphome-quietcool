#pragma once

#include "quietcool/core/core_types.h"

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
    if (size_ == kCapacity) return false;
    callbacks_[tail_] = StoredCoreCallback{kind, token};
    tail_ = (tail_ + 1U) % kCapacity;
    ++size_;
    return true;
  }

  std::optional<CoreCallback> pop(MonotonicMs now_ms) {
    if (size_ == 0) return std::nullopt;
    const auto callback = callbacks_[head_];
    callbacks_[head_].reset();
    head_ = (head_ + 1U) % kCapacity;
    --size_;
    return CoreCallback{callback->kind, callback->token, now_ms};
  }

 private:
  struct StoredCoreCallback final {
    CoreCallbackKind kind;
    std::optional<TxToken> token;
  };

  std::array<std::optional<StoredCoreCallback>, kCapacity> callbacks_{};
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t size_{0};
};

}  // namespace quietcool
