#pragma once

#include "frame_recovery.h"

namespace quietcool {

struct Consensus final {
  FanState state;
  SpeedCapability capability;
  std::uint8_t independent_candidates;
  EvidenceConfidence confidence;
};

struct ConsensusSnapshot final {
  bool has_group{false};
  std::uint8_t canonical_byte{0};
  std::uint8_t latest_raw_byte{0};
  SpeedCapability capability{SpeedCapability::Unknown};
  std::uint8_t independent_candidates{0};
  bool has_exact{false};
  MonotonicMs last_candidate_ms{0};
};

class ConsensusTracker final {
 public:
  ConsensusTracker() = default;
  // `own_outbound_byte` is the raw byte of the local transaction's own outbound
  // command frame, when one is in flight. A candidate byte-identical to it may
  // be the bridge's own echo (echoes are a documented field phenomenon here),
  // and an echo's marker bits (10) alias SpeedCapability::Two — so such frames
  // still count toward consensus (a genuine 2-speed fan's confirmation is
  // byte-identical to the echo, and semantic confirmation must keep working)
  // but contribute NO capability evidence (issue #31 review).
  std::optional<Consensus> observe(
      const RecoveredResponse& candidate, MonotonicMs now_ms,
      std::optional<std::uint8_t> own_outbound_byte = std::nullopt);
  void reset();
  ConsensusSnapshot snapshot() const;
 private:
  struct Group final {
    FanState state;
    SpeedCapability capability;
    std::uint8_t count;
    bool has_exact;
    MonotonicMs last_ms;
  };
  std::optional<Group> group_;
};

}  // namespace quietcool

