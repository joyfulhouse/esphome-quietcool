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
  std::optional<Consensus> observe(const RecoveredResponse& candidate,
                                   MonotonicMs now_ms);
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

