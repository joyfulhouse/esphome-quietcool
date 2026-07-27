#include "fan_feedback.h"

namespace esphome::quietcool {

FanFeedback authority_to_feedback(const ::quietcool::FanState& confirmed,
                                  std::uint8_t supported_speed_count) {
  FanFeedback feedback{};
  feedback.on = confirmed.is_on();
  // Wire nibble -> Home Assistant LEVEL, the inverse of speed_for_level: the
  // nibbles have fixed meanings (1=LOW, 2=MED, 3=HIGH) while the entity's band
  // is positional, so on a 2-speed (LOW/HIGH) fan a confirmed HIGH (nibble 3)
  // must publish as level 2 — publishing the raw nibble put 3 into a 2-level
  // entity, which Home Assistant cannot represent, so a remote HIGH press
  // showed nothing at all (issue #30). level = min(nibble, count) is total,
  // is the identity for 3-speed units, and folds an unexpected MED report on
  // a 2-speed fan to the top of the band rather than dropping the update.
  //
  // The band is the caller's — authority_speed_count() of the snapshot being
  // published — and never this report's own capability marker bits, which alias
  // Two on any command-shaped frame; see the header. Flooring at 1 keeps the
  // function total against a degenerate count (issue #19 floored the consumer
  // too).
  const std::uint8_t effective_count =
      supported_speed_count >= 1 ? supported_speed_count : 1;
  if (const auto confirmed_speed = confirmed.speed()) {
    const int nibble = static_cast<int>(*confirmed_speed);
    feedback.speed =
        nibble > static_cast<int>(effective_count)
            ? static_cast<int>(effective_count)
            : nibble;
  }
  return feedback;
}

std::uint8_t authority_speed_count(
    const ::quietcool::AuthoritySnapshot& authority) {
  // Producer guard: only a capability in the fan's real 1..3 band may become
  // the entity's speed count. The core never stores an out-of-band value
  // (restorable_state_is_valid and promote() both filter), so this is defence
  // in depth, not a live code path. A snapshot with no capability yields the
  // unknown-capability band — never a previously cached count, which after
  // learning a DIFFERENT fan would be the old fan's band and would keep
  // transmitting the wrong byte for a level press (issue #31 review).
  if (authority.speed_capability) {
    const auto count = static_cast<std::uint8_t>(*authority.speed_capability);
    if (count >= 1 && count <= 3) return count;
  }
  return kUnknownCapabilitySpeedCount;
}

}  // namespace esphome::quietcool
