#include "consensus_tracker.h"

namespace quietcool {
namespace {

std::uint8_t saturating_increment(std::uint8_t value) {
  return value == 0xFF ? value : static_cast<std::uint8_t>(value + 1);
}

}  // namespace

void ConsensusTracker::vote(Group& group, const FanState& state) {
  // Marker bits 10 alias SpeedCapability::Two, and they are what every command
  // frame carries: our own echo, and a paired OEM remote's bursts, which use
  // the sender ID we are bound to (issue #46). Such a frame is RANKED, not
  // discarded: a 2-speed fan's report is byte-identical to a command by
  // construction, so discarding it would make capability Two unlearnable
  // altogether (issue #31 review). The rank is a property of the frame's
  // shape, not of the window it was heard in.
  switch (state.report_capability().value_or(SpeedCapability::Unknown)) {
    case SpeedCapability::Unknown:break;
    case SpeedCapability::One:
      group.one_votes = saturating_increment(group.one_votes);break;
    case SpeedCapability::Two:group.command_shaped = true;break;
    case SpeedCapability::Three:
      group.three_votes = saturating_increment(group.three_votes);break;
  }
}

ConsensusTracker::Verdict ConsensusTracker::verdict(const Group& group) {
  // Precedence: a fan-only capability (bit 6 set, which no command constructor
  // produces) always outranks a command-shaped Two — a 3-speed fan answers a
  // 0xAF command with 0xEF, the same canonical byte, so both land in one group
  // and arrival order must not decide the band. Among fan-only values the
  // majority wins, and only a corroborated majority is Unambiguous: the two
  // state bytes matching is the protocol's sole integrity check and the link
  // is observed to corrupt both alike, so one frame proves nothing against a
  // capability that is already known (issue #46 follow-up).
  const auto one = group.one_votes;
  const auto three = group.three_votes;
  if (one != 0 || three != 0) {
    if (one == three) return {SpeedCapability::Unknown, CapabilityEvidence::Uncorroborated};
    const bool is_three = three > one;
    const auto votes = is_three ? three : one;
    return {is_three ? SpeedCapability::Three : SpeedCapability::One,
            votes >= 2 ? CapabilityEvidence::Unambiguous
                       : CapabilityEvidence::Uncorroborated};
  }
  if (group.command_shaped)
    return {SpeedCapability::Two, CapabilityEvidence::CommandShaped};
  return {SpeedCapability::Unknown, CapabilityEvidence::Uncorroborated};
}

ConsensusTracker::Group ConsensusTracker::open_group(
    const RecoveredResponse& candidate, MonotonicMs now_ms) {
  Group group{candidate.state, 0, 0, false, 1,
              candidate.quality == RecoveryQuality::Exact, now_ms};
  vote(group, candidate.state);
  return group;
}

std::optional<Consensus> ConsensusTracker::observe(
    const RecoveredResponse& candidate, MonotonicMs now_ms) {
  if (candidate.kind == ResponseKind::Special) return std::nullopt;
  if (!group_) {
    group_ = open_group(candidate, now_ms);
    return std::nullopt;
  }
  const auto age = elapsed_since(now_ms, group_->last_ms);
  if (!age) return std::nullopt;
  if (candidate.state.canonical_byte() != group_->state.canonical_byte()) {
    group_ = open_group(candidate, now_ms);
    return std::nullopt;
  }
  group_->state = candidate.state;
  group_->last_ms = now_ms;
  // Only an INDEPENDENT candidate counts or votes: a same-burst repeat is not
  // a second witness for the state, and it is not one for the capability.
  if (*age < kMinIndependentCandidateGapMs) return std::nullopt;
  if (group_->count != 0xFF) ++group_->count;
  vote(*group_, candidate.state);
  group_->has_exact = group_->has_exact || candidate.quality == RecoveryQuality::Exact;
  const std::uint8_t required = group_->has_exact ? 2 : 3;
  if (group_->count < required) return std::nullopt;
  // ExactBackedConsensus denotes observational corroboration (repeated
  // consistent frames), not authenticity; the OEM protocol is unauthenticated.
  // See SECURITY.md.
  const auto capability = verdict(*group_);
  return Consensus{group_->state, capability.capability, capability.evidence,
                   group_->count,
                   group_->has_exact ? EvidenceConfidence::ExactBackedConsensus
                                     : EvidenceConfidence::RecoveredOnlyConsensus};
}

void ConsensusTracker::reset() { group_.reset(); }

ConsensusSnapshot ConsensusTracker::snapshot() const {
  if (!group_) return {};
  const auto capability = verdict(*group_);
  return {true, group_->state.canonical_byte(), group_->state.raw_byte(),
          capability.capability, capability.evidence, group_->count,
          group_->has_exact, group_->last_ms};
}

}  // namespace quietcool

