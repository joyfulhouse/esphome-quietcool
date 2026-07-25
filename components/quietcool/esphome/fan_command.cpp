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

::quietcool::FanState fan_command_from_intent(bool on, int speed,
                                              std::uint8_t supported_speed_count) {
  const auto typed_speed = static_cast<::quietcool::Speed>(
      clamp_fan_speed(speed, supported_speed_count));
  const auto duration = on ? ::quietcool::Duration::Continuous
                           : ::quietcool::Duration::Off;
  return ::quietcool::FanState::command(typed_speed, duration);
}

}  // namespace esphome::quietcool
