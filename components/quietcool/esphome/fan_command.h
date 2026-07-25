#pragma once

#include "quietcool/core/fan_state.h"

#include <cstdint>

namespace esphome::quietcool {

// Clamps a Home Assistant speed level into the [1, max(1, supported_speed_count)]
// band the fan understands, so neither an out-of-range request nor a degenerate
// supported_speed_count of 0 can form an undefined speed nibble. Total and safe
// in isolation; callers still pass a count of 1..3 in normal operation.
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
