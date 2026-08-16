#include "recovery_scheduler.h"

namespace quietcool {

void RecoveryScheduler::arm_from_oem_activity(MonotonicMs now_ms) {
  const bool same_cycle = cause_ && *cause_ == RecoveryCause::OemActivity;
  cause_ = RecoveryCause::OemActivity;
  anchor_ms_ = now_ms;
  due_ms_ = saturating_add(now_ms, kOemRecoveryQuietMs);
  expires_ms_ = saturating_add(now_ms, kOemRecoveryMaxAgeMs);
  if (!same_cycle) queries_started_ = 0;
  phase_ = queries_started_ >= 2 ? RecoveryPhase::Complete
                                 : RecoveryPhase::QuietWait;
}

void RecoveryScheduler::arm_from_timer_expiry(MonotonicMs timer_deadline_ms) {
  cause_ = RecoveryCause::EstimatedTimerExpiry;
  anchor_ms_ = timer_deadline_ms;
  due_ms_ = saturating_add(timer_deadline_ms, jitter(0x54494D45U));
  expires_ms_ =
      saturating_add(timer_deadline_ms, kTimerExpiryRecoveryMaxAgeMs);
  queries_started_ = 0;
  phase_ = RecoveryPhase::QuietWait;
}

void RecoveryScheduler::cancel() {
  cause_.reset();
  phase_ = RecoveryPhase::Inactive;
  queries_started_ = 0;
}

RecoveryDue RecoveryScheduler::poll(MonotonicMs now_ms) const {
  if (!cause_ || phase_ == RecoveryPhase::Inactive)
    return {RecoveryDueStatus::NotDue, std::nullopt};
  if (now_ms < anchor_ms_) return {RecoveryDueStatus::NotDue, std::nullopt};
  // A spent cycle (Complete) still expires: fresh OEM activity can re-arm an
  // already-exhausted query budget into Complete, and the coordinator waiting in
  // RecoveryQuietWait must still return to Idle 30s after the latest evidence.
  if (now_ms > expires_ms_) return {RecoveryDueStatus::Expired, std::nullopt};
  if (phase_ == RecoveryPhase::Complete)
    return {RecoveryDueStatus::NotDue, std::nullopt};
  if (now_ms < due_ms_) return {RecoveryDueStatus::NotDue, std::nullopt};
  if (phase_ != RecoveryPhase::QuietWait && phase_ != RecoveryPhase::RetryWait)
    return {RecoveryDueStatus::NotDue, std::nullopt};
  if (*cause_ == RecoveryCause::EstimatedTimerExpiry)
    return {RecoveryDueStatus::QueryDue, TxReason::TimerExpiryRecoveryQuery};
  return {RecoveryDueStatus::QueryDue,
          queries_started_ == 0 ? TxReason::RecoveryQueryInitial
                                : TxReason::RecoveryQueryRetry};
}

void RecoveryScheduler::note_query_started(MonotonicMs) {
  if (!cause_ || queries_started_ == 0xFF) return;
  ++queries_started_;
  phase_ = queries_started_ == 1 ? RecoveryPhase::InitialResponse
                                 : RecoveryPhase::RetryResponse;
}

void RecoveryScheduler::note_consensus() {
  if (cause_) phase_ = RecoveryPhase::Complete;
}

void RecoveryScheduler::note_empty_window(MonotonicMs epoch_anchor_ms) {
  if (!cause_) return;
  if (*cause_ == RecoveryCause::EstimatedTimerExpiry || queries_started_ >= 2) {
    phase_ = RecoveryPhase::Complete;
    return;
  }
  due_ms_ = saturating_add(
      saturating_add(epoch_anchor_ms, kResponseTailEndMs),
      jitter(queries_started_));
  phase_ = RecoveryPhase::RetryWait;
}

RecoverySnapshot RecoveryScheduler::snapshot() const {
  return {cause_, phase_, anchor_ms_, due_ms_, expires_ms_, queries_started_};
}

MonotonicMs RecoveryScheduler::jitter(std::uint32_t salt) const {
  const auto mixed = deterministic_jitter_mix(jitter_seed_, salt, anchor_ms_);
  return 500U + mixed % 501U;
}

}  // namespace quietcool
