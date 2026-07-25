#pragma once

#include "command_transaction.h"
#include "consensus_tracker.h"

namespace quietcool {

struct ConsensusObservation final {
  FanState state;
  SpeedCapability capability;
  EvidenceConfidence confidence;
  EvidenceSource source;
};
struct OnRequestContext final {
  FanState requested;
  std::optional<PriorAuthoritySnapshot> prior_authority;
  bool attempts_remain;
};
struct OffRequestContext final {
  FanState requested;
  std::optional<PriorAuthoritySnapshot> prior_authority;
  bool attempts_remain;
};
struct QueryAcceptanceContext final { EvidenceSource source; };

struct ConfirmTransactionAndPromote final {};
struct RetryWithoutPromotion final {};
struct YieldToPossibleOem final {};
struct ExhaustAndPromoteObservedState final {};
struct ExhaustWithoutPromotion final {};
struct RetryAndPromoteObservedState final {};
struct PromoteWithoutTransaction final {};
struct DiagnosticOnly final {};

using OnRequestDecision = std::variant<ConfirmTransactionAndPromote,
    RetryWithoutPromotion, YieldToPossibleOem,
    ExhaustAndPromoteObservedState, ExhaustWithoutPromotion>;
using OffRequestDecision = std::variant<ConfirmTransactionAndPromote,
    RetryWithoutPromotion, RetryAndPromoteObservedState,
    ExhaustAndPromoteObservedState, ExhaustWithoutPromotion>;
using QueryDecision = std::variant<PromoteWithoutTransaction, DiagnosticOnly>;

class ObservationPolicy final {
 public:
  OnRequestDecision decide_on(const ConsensusObservation& observation,
                              const OnRequestContext& context) const;
  OffRequestDecision decide_off(const ConsensusObservation& observation,
                                const OffRequestContext& context) const;
  QueryDecision decide_query(const ConsensusObservation& observation,
                             const QueryAcceptanceContext& context) const;
};

}  // namespace quietcool

