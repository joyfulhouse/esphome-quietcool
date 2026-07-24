#include "command_transaction.h"

namespace quietcool {

CommandTransaction CommandTransaction::begin(
    TransactionId id, FanState requested,
    std::optional<PriorAuthoritySnapshot> prior_authority) {
  return CommandTransaction(id, requested, std::move(prior_authority),
                            requested.is_on() ? 4 : 6);
}

JoinDecision CommandTransaction::compare_request(FanState requested) const {
  return requested_.semantically_equals(requested) ? JoinDecision::SemanticDuplicate
                                                    : JoinDecision::Different;
}

Result<AttemptNumber, BudgetError> CommandTransaction::note_command_burst_started() {
  if (outcome_)
    return Result<AttemptNumber, BudgetError>::err(BudgetError::Terminal);
  if (!may_emit_another_command())
    return Result<AttemptNumber, BudgetError>::err(BudgetError::Exhausted);
  ++started_;
  fallback_used_ = false;
  return Result<AttemptNumber, BudgetError>::ok(AttemptNumber(started_));
}

bool CommandTransaction::may_emit_another_command() const {
  return !outcome_ && started_ < limit_;
}

RefireCount CommandTransaction::remaining_refires() const {
  const std::uint8_t refire_limit = static_cast<std::uint8_t>(limit_ - 1);
  const std::uint8_t spent_refires = started_ > 0 ?
      static_cast<std::uint8_t>(started_ - 1) : 0;
  return RefireCount(static_cast<std::uint8_t>(refire_limit - spent_refires));
}

void CommandTransaction::reaim_off_to(Speed reported_speed) {
  if (!requested_.is_on()) outbound_ = FanState::command(reported_speed, Duration::Off);
}

void CommandTransaction::finish(TransactionOutcome outcome) {
  if (!outcome_) outcome_ = outcome;
}

bool CommandTransaction::mark_fallback_used() {
  if (fallback_used_) return false;
  fallback_used_ = true;
  return true;
}

TransactionSnapshot CommandTransaction::snapshot() const {
  return {id_, requested_, outbound_, limit_, started_, remaining_refires(),
          prior_, outcome_, fallback_used_};
}

}  // namespace quietcool
