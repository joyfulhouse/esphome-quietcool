#include "fan_feedback.h"

namespace esphome::quietcool {

FanFeedback authority_to_feedback(const ::quietcool::FanState& confirmed,
                                  std::uint8_t current_supported_speed_count) {
  FanFeedback feedback{};
  feedback.on = confirmed.is_on();
  // Only a reported capability in the fan's real 1..3 band updates the speed
  // count. report_capability() also yields Unknown (0) when the top two bits are
  // clear; that is filtered here, so supported_speed_count never carries 0 and
  // the command-path clamp band stays in [1, 3]. This is a deliberate producer
  // guard, not just consumer defence (issue #19 floored the consumer too).
  if (const auto capability = confirmed.report_capability()) {
    const auto count = static_cast<std::uint8_t>(*capability);
    if (count >= 1 && count <= 3) feedback.supported_speed_count = count;
  }
  // Wire nibble -> Home Assistant LEVEL, the inverse of speed_for_level: the
  // nibbles have fixed meanings (1=LOW, 2=MED, 3=HIGH) while the entity's band
  // is positional, so on a 2-speed (LOW/HIGH) fan a confirmed HIGH (nibble 3)
  // must publish as level 2 — publishing the raw nibble put 3 into a 2-level
  // entity, which Home Assistant cannot represent, so a remote HIGH press
  // showed nothing at all (issue #30). level = min(nibble, count) is total,
  // is the identity for 3-speed units, and folds an unexpected MED report on
  // a 2-speed fan to the top of the band rather than dropping the update.
  // Prefer the capability carried by this same report so a report that both
  // narrows the band and carries a speed stays self-consistent.
  const std::uint8_t effective_count =
      feedback.supported_speed_count ? *feedback.supported_speed_count
      : current_supported_speed_count >= 1 ? current_supported_speed_count
                                           : 1;
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
    const ::quietcool::AuthoritySnapshot& authority,
    std::uint8_t current_supported_speed_count) {
  // Same producer guard as above: only a capability in the fan's real 1..3
  // band may become the entity's speed count. The core never stores an
  // out-of-band value (restorable_state_is_valid and promote() both filter),
  // so this is defence in depth, not a live code path.
  if (authority.speed_capability) {
    const auto count = static_cast<std::uint8_t>(*authority.speed_capability);
    if (count >= 1 && count <= 3) return count;
  }
  return current_supported_speed_count;
}

}  // namespace esphome::quietcool
