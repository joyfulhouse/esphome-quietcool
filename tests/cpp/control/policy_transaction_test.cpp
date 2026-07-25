#include "quietcool/core/command_transaction.h"
#include "quietcool/core/observation_policy.h"
#include "support/test.h"

#include <type_traits>
#include <variant>

namespace quietcool {
namespace {

PriorAuthoritySnapshot prior(FanState state) {
  return {state, EvidenceSource::ManualQueryConsensus, 100, true, 7};
}

ConsensusObservation observation(FanState state) {
  return {state, SpeedCapability::Three,
          EvidenceConfidence::ExactBackedConsensus,
          EvidenceSource::PostCommandConsensus};
}

QC_TEST("transaction", "ON and OFF budgets are fixed and spend only at start") {
  auto on = CommandTransaction::begin(
      TransactionId(1), FanState::command(Speed::Low, Duration::Continuous),
      std::nullopt);
  QC_CHECK_EQ(on.remaining_refires(), RefireCount(3));
  QC_CHECK_EQ(on.note_command_burst_started().value(), AttemptNumber(1));
  QC_CHECK_EQ(on.remaining_refires(), RefireCount(3));
  for (int attempt = 2; attempt <= 4; ++attempt)
    QC_CHECK_EQ(on.note_command_burst_started().value(), AttemptNumber(attempt));
  QC_CHECK(!on.may_emit_another_command());
  QC_CHECK(!on.note_command_burst_started().has_value());

  auto off = CommandTransaction::begin(
      TransactionId(2), FanState::command(Speed::Low, Duration::Off),
      std::nullopt);
  QC_CHECK_EQ(off.note_command_burst_started().value(), AttemptNumber(1));
  QC_CHECK_EQ(off.remaining_refires(), RefireCount(5));
}

QC_TEST("transaction", "duplicates join without renewing and different requests supersede") {
  auto transaction = CommandTransaction::begin(
      TransactionId(1), FanState::command(Speed::Low, Duration::Off),
      prior(FanState::observed(0xDF).value()));
  transaction.note_command_burst_started();
  QC_CHECK_EQ(transaction.compare_request(
                  FanState::command(Speed::High, Duration::Off)),
              JoinDecision::SemanticDuplicate);
  QC_CHECK_EQ(transaction.remaining_refires(), RefireCount(5));
  QC_CHECK_EQ(transaction.compare_request(
                  FanState::command(Speed::Low, Duration::Continuous)),
              JoinDecision::Different);
}

QC_TEST("transaction", "OFF re-aim changes wire variant but not semantic request") {
  const auto requested = FanState::command(Speed::Low, Duration::Off);
  auto transaction = CommandTransaction::begin(TransactionId(1), requested,
                                                std::nullopt);
  transaction.reaim_off_to(Speed::High);
  const auto snapshot = transaction.snapshot();
  QC_CHECK(snapshot.requested.semantically_equals(requested));
  QC_CHECK_EQ(snapshot.outbound_command,
              FanState::command(Speed::High, Duration::Off));
  transaction.finish(TransactionOutcome::Confirmed);
  transaction.finish(TransactionOutcome::Exhausted);
  QC_CHECK_EQ(transaction.snapshot().outcome.value(),
              TransactionOutcome::Confirmed);
}

QC_TEST("policy", "matching observations always confirm") {
  ObservationPolicy policy;
  const auto requested = FanState::command(Speed::Low, Duration::Continuous);
  const OnRequestContext context{requested, std::nullopt, true};
  QC_CHECK(std::holds_alternative<ConfirmTransactionAndPromote>(
      policy.decide_on(observation(FanState::observed(0xDF).value()), context)));
}

QC_TEST("policy", "ON command-shaped mismatch yields unless it is prior echo") {
  ObservationPolicy policy;
  const auto requested = FanState::command(Speed::Low, Duration::Continuous);
  const auto high_command = FanState::observed(0xBF).value();
  QC_CHECK(std::holds_alternative<YieldToPossibleOem>(policy.decide_on(
      observation(high_command), {requested, std::nullopt, true})));
  QC_CHECK(std::holds_alternative<RetryWithoutPromotion>(policy.decide_on(
      observation(high_command), {requested, prior(high_command), true})));
  QC_CHECK(std::holds_alternative<ExhaustWithoutPromotion>(policy.decide_on(
      observation(high_command), {requested, prior(high_command), false})));
}

QC_TEST("policy", "future ON work never promotes OFF or safe mismatch") {
  ObservationPolicy policy;
  const auto requested = FanState::command(Speed::Low, Duration::Continuous);
  QC_CHECK(std::holds_alternative<RetryWithoutPromotion>(policy.decide_on(
      observation(FanState::observed(0xC0).value()),
      {requested, std::nullopt, true})));
  QC_CHECK(std::holds_alternative<RetryWithoutPromotion>(policy.decide_on(
      observation(FanState::observed(0x2F).value()),
      {requested, std::nullopt, true})));
}

QC_TEST("policy", "OFF has no yield type and re-aims within fixed budget") {
  static_assert(std::variant_size<OffRequestDecision>::value == 5,
                "OFF decision must omit yield");
  ObservationPolicy policy;
  const auto requested = FanState::command(Speed::Low, Duration::Off);
  QC_CHECK(std::holds_alternative<RetryWithoutPromotion>(policy.decide_off(
      observation(FanState::observed(0xBF).value()),
      {requested, std::nullopt, true})));
  QC_CHECK(std::holds_alternative<RetryAndPromoteObservedState>(policy.decide_off(
      observation(FanState::observed(0xD1).value()),
      {requested, std::nullopt, true})));
}

QC_TEST("policy", "query consensus promotes normal observations") {
  ObservationPolicy policy;
  const auto decision = policy.decide_query(
      observation(FanState::observed(0xD1).value()),
      {EvidenceSource::ManualQueryConsensus});
  QC_CHECK(std::holds_alternative<PromoteWithoutTransaction>(decision));
}

}  // namespace
}  // namespace quietcool
