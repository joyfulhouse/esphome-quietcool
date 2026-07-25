#include "fan_command.h"

namespace esphome::quietcool {

int clamp_fan_speed(int speed, std::uint8_t supported_speed_count) {
  if (speed < 1) return 1;
  if (speed > supported_speed_count) return supported_speed_count;
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
