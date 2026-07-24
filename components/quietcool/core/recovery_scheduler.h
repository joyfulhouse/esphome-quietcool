#pragma once

#include "core_types.h"

namespace quietcool {

enum class RecoveryCause : std::uint8_t { OemActivity, EstimatedTimerExpiry };
enum class RecoveryPhase : std::uint8_t {
  Inactive, QuietWait, InitialQueryPending, InitialResponse, RetryWait,
  RetryQueryPending, RetryResponse, Complete, Expired
};
enum class RecoveryDueStatus : std::uint8_t { NotDue, QueryDue, Expired };
struct RecoveryDue final {
  RecoveryDueStatus status;
  std::optional<TxReason> reason;
};
struct RecoverySnapshot final {
  std::optional<RecoveryCause> cause;
  RecoveryPhase phase;
  MonotonicMs cause_anchor_ms;
  MonotonicMs due_ms;
  MonotonicMs expires_ms;
  std::uint8_t queries_started;
};

class RecoveryScheduler final {
 public:
  explicit RecoveryScheduler(std::uint32_t jitter_seed = 0)
      : jitter_seed_(jitter_seed) {}
  void arm_from_oem_activity(MonotonicMs now_ms);
  void arm_from_timer_expiry(MonotonicMs timer_deadline_ms);
  void cancel();
  RecoveryDue poll(MonotonicMs now_ms) const;
  void note_query_started(MonotonicMs now_ms);
  void note_consensus();
  void note_empty_window(MonotonicMs epoch_anchor_ms);
  RecoverySnapshot snapshot() const;
 private:
  MonotonicMs jitter(std::uint32_t salt) const;
  std::optional<RecoveryCause> cause_;
  RecoveryPhase phase_{RecoveryPhase::Inactive};
  MonotonicMs anchor_ms_{0};
  MonotonicMs due_ms_{0};
  MonotonicMs expires_ms_{0};
  std::uint8_t queries_started_{0};
  std::uint32_t jitter_seed_;
};

}  // namespace quietcool
