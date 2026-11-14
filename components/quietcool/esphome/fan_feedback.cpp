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
  // The band is the caller's — entity_speed_count() of the capability being
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

namespace {

// Producer guard shared by both bands: only a capability in the fan's real
// 1..3 band may become a speed count. The core never stores an out-of-band
// value (restorable_state_is_valid and promote() both filter), so this is
// defence in depth, not a live code path. Absent or out-of-band capability
// yields nothing and each band falls back to its own unlearned default — never
// to a previously cached count, which after learning a DIFFERENT fan would be
// the old fan's band and would keep transmitting the wrong byte for a level
// press (issue #31 review).
std::optional<std::uint8_t> in_band_count(
    std::optional<::quietcool::SpeedCapability> capability) {
  if (!capability) return std::nullopt;
  const auto count = static_cast<std::uint8_t>(*capability);
  if (count < 1 || count > 3) return std::nullopt;
  return count;
}

}  // namespace

std::uint8_t entity_speed_count(
    std::optional<::quietcool::SpeedCapability> capability) {
  // Unknown -> the WIDEST band, so every capability the fan can later confirm
  // narrows the entity rather than widening it. Home Assistant caches this
  // count from ListEntities for the life of the connection, and only narrowing
  // survives that cache; see the header.
  if (const auto count = in_band_count(capability)) return *count;
  return kUnlearnedEntitySpeedCount;
}

std::uint8_t command_speed_count(
    std::optional<::quietcool::SpeedCapability> capability) {
  // Unknown -> the widest band that cannot form MED, the one speed a 2-speed
  // fan lacks. Nothing caches this count, so unlike the entity band it is free
  // to widen the moment a confirmed report proves three speeds.
  if (const auto count = in_band_count(capability)) return *count;
  return kUnlearnedCommandSpeedCount;
}

void FanSpeedBands::observe(
    std::optional<::quietcool::SpeedCapability> capability) {
  // The latch, and the only band state that is not a pure function of the
  // current capability: the entity band may narrow but never widen, because
  // Home Assistant cached it at ListEntities and cannot be told otherwise
  // within the connection. Nothing in here can observe a connection, so the
  // latch is released from outside — relist(), or a reboot. See the header.
  capability_ = capability;
  const std::uint8_t entity = entity_speed_count(capability);
  if (entity < entity_) entity_ = entity;
  // Bounded by the latched entity band, never latched itself: an absent
  // capability must be able to pull it back down to the MED-free band after a
  // re-bind, and a confirmed Three must be able to raise it as far as the band
  // Home Assistant is actually working in.
  const std::uint8_t command = command_speed_count(capability);
  command_ = command < entity_ ? command : entity_;
}

bool FanSpeedBands::relist() {
  // Both bands become pure functions of the last observed capability again.
  // Narrowing is already applied eagerly by observe(), so the only change this
  // can make is a widening — the one move that needs the caller's guarantee
  // that no listing of the old band is still held (issue #46).
  const std::uint8_t before = entity_;
  entity_ = entity_speed_count(capability_);
  command_ = command_speed_count(capability_);
  return entity_ != before;
}

}  // namespace esphome::quietcool
