#include "fan_command.h"

namespace esphome::quietcool {

int clamp_fan_speed(int speed, std::uint8_t supported_speed_count) {
  // Floor the ceiling at 1: a supported_speed_count of 0 would otherwise let
  // speed 0 through, which casts to an undefined Speed nibble. Keeping the
  // function total makes it safe in isolation rather than by a caller
  // precondition in a different file (issue #19).
  const int ceiling =
      supported_speed_count < 1 ? 1 : static_cast<int>(supported_speed_count);
  if (speed < 1) return 1;
  if (speed > ceiling) return ceiling;
  return speed;
}

::quietcool::Speed speed_for_level(int level, std::uint8_t supported_speed_count) {
  // The wire nibbles have FIXED meanings — 1=LOW (0x9F on the wire), 2=MED
  // (0xAF), 3=HIGH (0xBF); see the legacy build's byte table — and a fan
  // supports a SUBSET of them. A 2-speed QuietCool is LOW/HIGH, i.e. {1, 3}:
  // its own remote sends 0xBF for HIGH. So the Home Assistant level band maps
  // by POSITION, not by identity: the top of the band is always High and the
  // bottom is always Low. An identity cast here is exactly the bug that made
  // "high" on a 2-speed fan transmit MED (0xAF) — a speed the fan does not
  // have — which stopped it instead (issue #30).
  //
  // Over the real 1..3 domain (production initialises at 3 and accepts only
  // reported counts 1..3), count==3 degenerates to the identity mapping
  // (1→Low, 2→Medium, 3→High), so 3-speed units are unaffected. Outside that
  // domain the rule is positional, not identity — deliberate, since a count
  // above 3 has no wire meaning to be identical to. count==1 maps its single level to High
  // by the same top-of-band rule; no 1-speed unit exists here to verify
  // against, and that choice is recorded rather than hidden.
  const int clamped = clamp_fan_speed(level, supported_speed_count);
  const int ceiling =
      supported_speed_count < 1 ? 1 : static_cast<int>(supported_speed_count);
  if (clamped >= ceiling) return ::quietcool::Speed::High;
  if (clamped == 1) return ::quietcool::Speed::Low;
  return ::quietcool::Speed::Medium;
}

::quietcool::FanState fan_command_from_intent(bool on, int speed,
                                              std::uint8_t supported_speed_count) {
  const auto typed_speed = speed_for_level(speed, supported_speed_count);
  const auto duration = on ? ::quietcool::Duration::Continuous
                           : ::quietcool::Duration::Off;
  return ::quietcool::FanState::command(typed_speed, duration);
}

}  // namespace esphome::quietcool
