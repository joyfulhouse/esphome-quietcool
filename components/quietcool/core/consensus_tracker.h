#pragma once

#include "frame_recovery.h"

namespace quietcool {

struct Consensus final {
  FanState state;
  SpeedCapability capability;
  // Rank of `capability`, carried to AuthorityStore::promote so the sticky
  // value can prefer evidence that cannot have come from our own radio.
  CapabilityEvidence capability_evidence;
  std::uint8_t independent_candidates;
  EvidenceConfidence confidence;
};

struct ConsensusSnapshot final {
  bool has_group{false};
  std::uint8_t canonical_byte{0};
  std::uint8_t latest_raw_byte{0};
  SpeedCapability capability{SpeedCapability::Unknown};
  CapabilityEvidence capability_evidence{CapabilityEvidence::Unambiguous};
  std::uint8_t independent_candidates{0};
  bool has_exact{false};
  MonotonicMs last_candidate_ms{0};
};

class ConsensusTracker final {
 public:
  ConsensusTracker() = default;
  // `own_outbound_byte` is the raw byte of the state frame this epoch's burst
  // actually transmitted, supplied ONLY in a post-command window — the only
  // window a state frame of ours opens. A candidate matching it may be the
  // bridge's own echo (echoes are a documented field phenomenon here) and an
  // echo's marker bits (10) alias SpeedCapability::Two, so such a candidate is
  // ranked CapabilityEvidence::PossiblyOwnEcho. It still groups, counts and
  // confirms — a genuine 2-speed fan's confirmation is byte-identical to the
  // echo, and both semantic confirmation and 2-speed band learning depend on
  // it — but within a group any Unambiguous capability outranks it, so a
  // 3-speed fan's own report in the same window wins (issue #31 review).
  std::optional<Consensus> observe(
      const RecoveredResponse& candidate, MonotonicMs now_ms,
      std::optional<std::uint8_t> own_outbound_byte = std::nullopt);
  void reset();
  ConsensusSnapshot snapshot() const;
 private:
  struct Group final {
    FanState state;
    SpeedCapability capability;
    CapabilityEvidence capability_evidence;
    std::uint8_t count;
    bool has_exact;
    MonotonicMs last_ms;
  };
  std::optional<Group> group_;
};

}  // namespace quietcool

