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
  // A candidate carrying marker bits 10 may be a command — the bridge's own
  // echo or the paired OEM remote's — rather than a 2-speed fan's report, so
  // it is ranked CapabilityEvidence::CommandShaped in EVERY receive context
  // (issue #46). It still groups, counts and confirms — a genuine 2-speed
  // fan's confirmation is byte-identical, and both semantic confirmation and
  // 2-speed band learning depend on it — but within a group any fan-only
  // capability outranks it, so a 3-speed fan's own report in the same window
  // wins (issue #31 review).
  //
  // Fan-only capabilities (bit 6 set: One, Three) are VOTED: each independent
  // candidate casts one vote, and the consensus reports the value only as
  // Unambiguous when it holds at least two votes and a strict majority. One
  // vote is Uncorroborated; a tie is no capability at all. A single corrupted
  // reply therefore cannot re-teach a known band (issue #46 follow-up).
  std::optional<Consensus> observe(const RecoveredResponse& candidate,
                                   MonotonicMs now_ms);
  void reset();
  ConsensusSnapshot snapshot() const;
 private:
  struct Group final {
    FanState state;
    std::uint8_t one_votes;
    std::uint8_t three_votes;
    bool command_shaped;
    std::uint8_t count;
    bool has_exact;
    MonotonicMs last_ms;
  };
  struct Verdict final {
    SpeedCapability capability;
    CapabilityEvidence evidence;
  };
  static Group open_group(const RecoveredResponse& candidate, MonotonicMs now_ms);
  static void vote(Group& group, const FanState& state);
  static Verdict verdict(const Group& group);
  std::optional<Group> group_;
};

}  // namespace quietcool

