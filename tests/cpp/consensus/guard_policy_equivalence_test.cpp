// The consensus decision is encoded twice: once as guard predicates in
// ConfirmationCore::transaction_guard_matches (which select the rule, and
// therefore the action), and once as variant outcomes in ObservationPolicy
// (which apply_consensus consults as a cross-check). apply_consensus compares
// them and, on disagreement, must fail safe.
//
// Nothing in the type system keeps the two encodings in step. This test pins
// their correspondence over the whole realizable input domain so that drift
// fails CI rather than reaching hardware.
//
// Why this matters more than an ordinary duplication: the consensus inputs are
// attacker-controllable. Any station in RF range can inject response frames
// during the listening window and choose which combination the coordinator
// evaluates. If the two encodings ever disagree for even one combination, that
// combination becomes remotely selectable.
//
// The test drives the REAL ObservationPolicy and the REAL guard table. It
// deliberately does not re-implement either — a third encoding would drift too.

#include "quietcool/core/confirmation_core.h"
#include "quietcool/core/fan_state.h"
#include "quietcool/core/observation_policy.h"
#include "quietcool/core/transition_table.h"
#include "support/test.h"

#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
#include <variant>
#include <vector>

namespace quietcool {
namespace {

FanState on_without_marker(Speed speed, Duration duration) {
  const auto raw = static_cast<std::uint8_t>(
      (static_cast<std::uint8_t>(speed) << 4) |
      static_cast<std::uint8_t>(duration));
  return FanState::observed(raw).value();
}

// Every state a consensus observation or a request can realistically take,
// chosen to span on/off, marker/no-marker, and differing speeds so that the
// derived tuple reaches all of its realizable values.
std::vector<FanState> candidate_states() {
  return {
      FanState::observed(0x00).value(),                    // off, no marker
      FanState::command(Speed::Low, Duration::Off),        // off, marker
      on_without_marker(Speed::Low, Duration::Hours1),     // on, no marker
      on_without_marker(Speed::High, Duration::Continuous),
      FanState::command(Speed::Low, Duration::Hours1),     // on, marker
      FanState::command(Speed::High, Duration::Continuous),
  };
}

PriorAuthoritySnapshot prior_of(const FanState& state, bool valid) {
  return PriorAuthoritySnapshot{state, EvidenceSource::PostCommandConsensus,
                                MonotonicMs(1000), valid, 1};
}

// Mirrors ConfirmationCore::guard_matches' construction of the tuple exactly
// (confirmation_reducer.cpp). Kept adjacent to the assertions so a change in
// the production mapping is visible here.
TransactionConsensusInput tuple_for(
    const FanState& observed, const FanState& requested,
    const std::optional<PriorAuthoritySnapshot>& prior, bool attempts_remain) {
  const bool prior_equal = prior && prior->valid_at_capture &&
                           observed.semantically_equals(prior->state);
  return TransactionConsensusInput{
      observed.semantically_equals(requested),
      requested.is_on(),
      observed.is_on(),
      observed.has_outbound_command_marker(),
      prior_equal ? PriorRelation::Equal
                  : (prior ? PriorRelation::Unequal : PriorRelation::Absent),
      attempts_remain};
}

// The action/decision correspondence apply_consensus enforces. This is the
// contract under test: if either encoding changes meaning, this must be
// updated deliberately rather than drifting.
bool decision_permits_action(const OnRequestDecision& decision, ActionId action) {
  if (std::holds_alternative<ConfirmTransactionAndPromote>(decision))
    return action == ActionId::ConfirmAndPromote;
  if (std::holds_alternative<YieldToPossibleOem>(decision))
    return action == ActionId::YieldToOem;
  if (std::holds_alternative<RetryWithoutPromotion>(decision))
    return action == ActionId::RetryWithoutPromotion ||
           action == ActionId::ApplyMismatchWithRetry;
  return action == ActionId::ExhaustMismatch;
}

bool decision_permits_action(const OffRequestDecision& decision, ActionId action) {
  if (std::holds_alternative<ConfirmTransactionAndPromote>(decision))
    return action == ActionId::ConfirmAndPromote;
  if (std::holds_alternative<RetryAndPromoteObservedState>(decision) ||
      std::holds_alternative<RetryWithoutPromotion>(decision))
    return action == ActionId::ApplyMismatchWithRetry;
  return action == ActionId::ExhaustMismatch;
}

using TupleKey = std::tuple<bool, bool, bool, bool, std::uint8_t, bool>;

TupleKey key_of(const TransactionConsensusInput& input) {
  return {input.semantic_match, input.request_on,
          input.consensus_on,   input.command_marker,
          static_cast<std::uint8_t>(input.prior_relation),
          input.attempts_remain};
}

void check_state(CoordinatorState state, std::set<TupleKey>& covered) {
  const ObservationPolicy policy;
  for (const auto& observed : candidate_states()) {
    for (const auto& requested : candidate_states()) {
      std::vector<std::optional<PriorAuthoritySnapshot>> priors{
          std::nullopt, prior_of(observed, true), prior_of(observed, false)};
      for (const auto& other : candidate_states())
        if (!other.semantically_equals(observed))
          priors.push_back(prior_of(other, true));

      for (const auto& prior : priors) {
        for (const bool attempts_remain : {false, true}) {
          const auto input =
              tuple_for(observed, requested, prior, attempts_remain);
          covered.insert(key_of(input));

          const auto matches =
              ConfirmationCore::matching_transaction_rules_for_test(state, input);

          // Exactly one rule must match: a gap strands the coordinator, an
          // overlap makes the outcome depend on table order.
          QC_CHECK_EQ(matches.count, std::size_t(1));

          const ConsensusObservation observation{
              observed, SpeedCapability::Three,
              EvidenceConfidence::ExactBackedConsensus,
              EvidenceSource::PostCommandConsensus};

          const bool agrees =
              requested.is_on()
                  ? decision_permits_action(
                        policy.decide_on(observation,
                                         {requested, prior, attempts_remain}),
                        matches.first.action)
                  : decision_permits_action(
                        policy.decide_off(observation,
                                          {requested, prior, attempts_remain}),
                        matches.first.action);

          // A failure here means the guard table and ObservationPolicy have
          // drifted. Fix the divergence; do NOT relax this assertion. The
          // fail-safe branch in apply_consensus exists to survive exactly this
          // condition on hardware, but it costs authority and a transaction.
          QC_CHECK(agrees);
        }
      }
    }
  }
}

QC_TEST("consensus", "guard table and ObservationPolicy agree on every input") {
  std::set<TupleKey> covered;
  check_state(CoordinatorState::PostCommandListening, covered);
  check_state(CoordinatorState::FallbackResponseListening, covered);

  // Guards against the enumeration silently narrowing: semantic_match implies
  // request_on == consensus_on, so the realizable domain is smaller than the
  // 96 abstract combinations, but it must not collapse further than this.
  QC_CHECK(covered.size() >= 30);
}

}  // namespace
}  // namespace quietcool
