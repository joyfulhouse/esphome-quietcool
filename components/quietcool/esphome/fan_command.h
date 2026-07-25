#pragma once

#include "quietcool/core/fan_state.h"

#include <cstdint>

namespace esphome::quietcool {

// Clamps a Home Assistant speed level into the [1, supported_speed_count] band
// the fan understands, so an out-of-range request can never form an undefined
// speed nibble. supported_speed_count is itself bounded to 1..3 by the caller.
int clamp_fan_speed(int speed, std::uint8_t supported_speed_count);

// Translates a Home Assistant fan intent — on/off plus a speed level — into the
// FanState command the confirmation core drives onto the RF link. ON becomes a
// running (Continuous) command; OFF becomes a stopped (Off) command. This is the
// whole of the actuation mapping, deliberately kept free of any ESPHome
// dependency so it can be unit-tested directly: an inversion here would turn a
// user's "turn on" into a fan that shuts off.
::quietcool::FanState fan_command_from_intent(bool on, int speed,
                                              std::uint8_t supported_speed_count);

}  // namespace esphome::quietcool
