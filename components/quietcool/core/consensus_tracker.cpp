#include "consensus_tracker.h"

namespace quietcool {
namespace {

SpeedCapability capability_of(const FanState& state) {
  return state.report_capability().value_or(SpeedCapability::Unknown);
}

}  // namespace

std::optional<Consensus> ConsensusTracker::observe(
    const RecoveredResponse& candidate, MonotonicMs now_ms) {
  if (candidate.kind == ResponseKind::Special) return std::nullopt;
  const auto capability = capability_of(candidate.state);
  if (!group_) {
    group_ = Group{candidate.state, capability, 1,
                   candidate.quality == RecoveryQuality::Exact, now_ms};
    return std::nullopt;
  }
  const auto age = elapsed_since(now_ms, group_->last_ms);
  if (!age) return std::nullopt;
  if (candidate.state.canonical_byte() != group_->state.canonical_byte()) {
    group_ = Group{candidate.state, capability, 1,
                   candidate.quality == RecoveryQuality::Exact, now_ms};
    return std::nullopt;
  }
  group_->state = candidate.state;
  if (capability != SpeedCapability::Unknown) group_->capability = capability;
  group_->last_ms = now_ms;
  if (*age < kMinIndependentCandidateGapMs) return std::nullopt;
  if (group_->count != 0xFF) ++group_->count;
  group_->has_exact = group_->has_exact || candidate.quality == RecoveryQuality::Exact;
  const std::uint8_t required = group_->has_exact ? 2 : 3;
  if (group_->count < required) return std::nullopt;
  return Consensus{group_->state, group_->capability, group_->count,
                   group_->has_exact ? EvidenceConfidence::ExactBackedConsensus
                                     : EvidenceConfidence::RecoveredOnlyConsensus};
}

void ConsensusTracker::reset() { group_.reset(); }

ConsensusSnapshot ConsensusTracker::snapshot() const {
  if (!group_) return {};
  return {true, group_->state.canonical_byte(), group_->state.raw_byte(),
          group_->capability, group_->count, group_->has_exact, group_->last_ms};
}

}  // namespace quietcool

