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
  // Marker bits 10 alias SpeedCapability::Two, and they are what every command
  // frame carries: our own echo, and the paired OEM remote's bursts, which use
  // the sender ID we are bound to and are heard in passive observation and in
  // the recovery-query windows the remote's own activity arms (issue #46).
  // Such a frame is RANKED, not discarded: a 2-speed fan's report is
  // byte-identical to a command by construction, so discarding it would make
  // capability Two unlearnable altogether (issue #31 review). The rank is a
  // property of the frame's shape, not of the window it was heard in — the
  // window can rule out our own echo, never the remote.
  const auto evidence = candidate.state.has_outbound_command_marker()
                            ? CapabilityEvidence::CommandShaped
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
  // Precedence inside the group: an Unambiguous capability always wins, a
  // command-shaped one only fills a gap. A 3-speed fan answers our
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

