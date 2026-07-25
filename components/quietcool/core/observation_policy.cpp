#include "observation_policy.h"

namespace quietcool {
namespace {

bool matches_prior(const ConsensusObservation& observation,
                   const std::optional<PriorAuthoritySnapshot>& prior) {
  return prior && prior->valid_at_capture &&
         observation.state.semantically_equals(prior->state);
}

}  // namespace

OnRequestDecision ObservationPolicy::decide_on(
    const ConsensusObservation& observation,
    const OnRequestContext& context) const {
  if (observation.state.semantically_equals(context.requested))
    return ConfirmTransactionAndPromote{};
  const bool marker = observation.state.has_outbound_command_marker();
  const bool prior = matches_prior(observation, context.prior_authority);
  if (observation.state.is_on() && marker && !prior)
    return YieldToPossibleOem{};
  if (context.attempts_remain) return RetryWithoutPromotion{};
  if (marker) return ExhaustWithoutPromotion{};
  return ExhaustAndPromoteObservedState{};
}

OffRequestDecision ObservationPolicy::decide_off(
    const ConsensusObservation& observation,
    const OffRequestContext& context) const {
  if (observation.state.semantically_equals(context.requested))
    return ConfirmTransactionAndPromote{};
  const bool marker = observation.state.has_outbound_command_marker();
  if (context.attempts_remain) {
    if (!marker && observation.state.is_on()) return RetryAndPromoteObservedState{};
    return RetryWithoutPromotion{};
  }
  if (marker) return ExhaustWithoutPromotion{};
  return ExhaustAndPromoteObservedState{};
}

QueryDecision ObservationPolicy::decide_query(
    const ConsensusObservation&, const QueryAcceptanceContext&) const {
  return PromoteWithoutTransaction{};
}

}  // namespace quietcool
