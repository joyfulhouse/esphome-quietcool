#include "consensus_tracker.h"

namespace quietcool {
namespace {

SpeedCapability capability_of(const FanState& state) {
  return state.report_capability().value_or(SpeedCapability::Unknown);
}

}  // namespace

std::optional<Consensus> ConsensusTracker::observe(
    const RecoveredResponse& candidate, MonotonicMs now_ms,
    std::optional<std::uint8_t> own_outbound_byte) {
  if (candidate.kind == ResponseKind::Special) return std::nullopt;
  const auto capability = capability_of(candidate.state);
  // A frame byte-identical to the state frame this epoch transmitted may be
  // our own echo, whose outbound marker bits (10) alias SpeedCapability::Two.
  // It is RANKED, not discarded: a 2-speed fan's confirming report is
  // byte-identical to the command by construction, so discarding it would make
  // capability Two unlearnable on the command path altogether — and outside a
  // response window a report classifies as ExternalPriorityState, which
  // invalidates authority rather than promoting it, so there is no other
  // learning path for an unsolicited report either (issue #31 review).
  const auto evidence =
      own_outbound_byte && candidate.state.raw_byte() == *own_outbound_byte
          ? CapabilityEvidence::PossiblyOwnEcho
          : CapabilityEvidence::Unambiguous;
  if (!group_) {
    group_ = Group{candidate.state, capability, evidence, 1,
                   candidate.quality == RecoveryQuality::Exact, now_ms};
    return std::nullopt;
  }
  const auto age = elapsed_since(now_ms, group_->last_ms);
  if (!age) return std::nullopt;
  if (candidate.state.canonical_byte() != group_->state.canonical_byte()) {
    group_ = Group{candidate.state, capability, evidence, 1,
                   candidate.quality == RecoveryQuality::Exact, now_ms};
    return std::nullopt;
  }
  group_->state = candidate.state;
  // Precedence inside the group: an Unambiguous capability always wins, an
  // echo-indistinguishable one only fills a gap. A 3-speed fan answers our
  // 0xAF command with 0xEF — a different raw byte but the same canonical byte,
  // so it joins this very group — and its Three must beat the echo's aliased
  // Two whichever of the two frames arrives first.
  if (capability != SpeedCapability::Unknown &&
      (evidence == CapabilityEvidence::Unambiguous ||
       group_->capability == SpeedCapability::Unknown)) {
    group_->capability = capability;
    group_->capability_evidence = evidence;
  }
  group_->last_ms = now_ms;
  if (*age < kMinIndependentCandidateGapMs) return std::nullopt;
  if (group_->count != 0xFF) ++group_->count;
  group_->has_exact = group_->has_exact || candidate.quality == RecoveryQuality::Exact;
  const std::uint8_t required = group_->has_exact ? 2 : 3;
  if (group_->count < required) return std::nullopt;
  // ExactBackedConsensus denotes observational corroboration (repeated
  // consistent frames), not authenticity; the OEM protocol is unauthenticated.
  // See SECURITY.md.
  return Consensus{group_->state, group_->capability,
                   group_->capability_evidence, group_->count,
                   group_->has_exact ? EvidenceConfidence::ExactBackedConsensus
                                     : EvidenceConfidence::RecoveredOnlyConsensus};
}

void ConsensusTracker::reset() { group_.reset(); }

ConsensusSnapshot ConsensusTracker::snapshot() const {
  if (!group_) return {};
  return {true, group_->state.canonical_byte(), group_->state.raw_byte(),
          group_->capability, group_->capability_evidence, group_->count,
          group_->has_exact, group_->last_ms};
}

}  // namespace quietcool

