#include "response_window.h"

namespace quietcool {

ResponseWindow ResponseWindow::post_command(TransactionId transaction,
                                             AttemptNumber attempt,
                                             MonotonicMs completed_ms) {
  return ResponseWindow(PostCommandEpoch{transaction, attempt}, completed_ms,
                        kPostCommandAcceptStartMs, kPostCommandAcceptEndMs,
                        kResponseTailEndMs);
}

ResponseWindow ResponseWindow::query(QueryPurpose purpose, TxToken token,
                                     MonotonicMs started_ms) {
  switch (purpose) {
    case QueryPurpose::Boot:
      return ResponseWindow(BootQueryEpoch{token}, started_ms,
          kDirectQueryAcceptStartMs, kDirectQueryAcceptEndMs, kResponseTailEndMs);
    case QueryPurpose::Manual:
      return ResponseWindow(ManualQueryEpoch{token}, started_ms,
          kDirectQueryAcceptStartMs, kDirectQueryAcceptEndMs, kResponseTailEndMs);
    case QueryPurpose::Fallback:
      return ResponseWindow(FallbackQueryEpoch{token}, started_ms,
          kDirectQueryAcceptStartMs, kDirectQueryAcceptEndMs, kResponseTailEndMs);
    case QueryPurpose::Recovery:
      return ResponseWindow(RecoveryQueryEpoch{token}, started_ms,
          kDirectQueryAcceptStartMs, kDirectQueryAcceptEndMs, kResponseTailEndMs);
  }
  return ResponseWindow(BootQueryEpoch{token}, started_ms,
      kDirectQueryAcceptStartMs, kDirectQueryAcceptEndMs, kResponseTailEndMs);
}

ResponseWindow ResponseWindow::classification_tail(MonotonicMs anchor_ms) {
  return ResponseWindow(TailEpoch{anchor_ms}, anchor_ms, 0, 0,
                        kResponseTailEndMs);
}

WindowPosition ResponseWindow::position_at(MonotonicMs now_ms) const {
  const auto age = elapsed_since(now_ms, anchor_ms_);
  if (!age || *age < accept_start_) return WindowPosition::BeforeAcceptance;
  if (*age <= accept_end_) return WindowPosition::Accepting;
  if (*age <= tail_end_) return WindowPosition::ClassificationTail;
  return WindowPosition::Expired;
}

bool ResponseWindow::accepts_at(MonotonicMs now_ms) const {
  return position_at(now_ms) == WindowPosition::Accepting;
}

bool ResponseWindow::classifies_at(MonotonicMs now_ms) const {
  const auto position = position_at(now_ms);
  return position == WindowPosition::Accepting ||
         position == WindowPosition::ClassificationTail;
}

bool ResponseWindow::is_post_command() const {
  return std::holds_alternative<PostCommandEpoch>(epoch_);
}

}  // namespace quietcool
