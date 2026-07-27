#include "command_transaction.h"

namespace quietcool {
namespace {

// The wire nibbles have FIXED meanings (1=LOW, 2=MED, 3=HIGH) and a fan
// supports a SUBSET of them: a 2-speed fan is {LOW, HIGH}, a 1-speed fan is
// {HIGH} (its single position is the top of the band, matching
// speed_for_level). Unknown filters nothing — no confirmed capability means
// no ground to second-guess the request.
bool speed_supported(Speed speed, SpeedCapability capability) {
  switch (capability) {
    case SpeedCapability::Unknown: case SpeedCapability::Three:return true;
    case SpeedCapability::Two:return speed != Speed::Medium;
    case SpeedCapability::One:return speed == Speed::High;
    default:return true;
  }
}

}  // namespace

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

void CommandTransaction::reaim_to_capability(SpeedCapability capability) {
  // Issue #31: a command created before the fan's capability was known can
  // carry a speed the fan lacks (MED on a 2-speed fan stops it, issue #30) —
  // permanently, because the FanState byte is frozen at command time. Re-aim
  // every attempt to the top of the band once capability is confirmed.
  // For an ON command the REQUESTED state is rewritten too: the corrected
  // speed is what the fan will report, and confirmation compares the report
  // against requested_ — leaving Medium there would misread the fan's High
  // report as an OEM override and yield. OFF requests stay as-is (semantic
  // equality for OFF ignores the speed nibble, mirroring reaim_off_to).
  const auto speed = outbound_.speed();
  if (!speed || speed_supported(*speed, capability)) return;
  outbound_ = FanState::command(Speed::High, outbound_.duration());
  if (requested_.is_on())
    requested_ = FanState::command(Speed::High, requested_.duration());
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
