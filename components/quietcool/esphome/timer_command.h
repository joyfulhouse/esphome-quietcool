#pragma once

#include "quietcool/core/fan_state.h"

#include <cstdint>

namespace esphome::quietcool {

// The durations a user may select for the fan to RUN. Deliberately not
// ::quietcool::Duration: that enum includes Off, which STOPS the fan (see
// Duration's own comment and the design doc §1). Reusing it here would let a
// future caller turn a timer request into a stop command — issue #30's failure
// shape by another route. Stopping the fan belongs to the fan entity alone.
enum class TimerSelection : std::uint8_t {
  Continuous, Hours1, Hours2, Hours4, Hours8, Hours12
};

// Maps a selection onto its wire duration nibble. Total; never yields Off.
::quietcool::Duration duration_for_selection(TimerSelection selection);

// Translates a timer selection into the FanState command driven onto the RF
// link. A timer command is speed|duration in one byte, so it is ENERGIZING: on
// a stopped fan it starts it, at LOW, matching the legacy YAML build. The speed
// nibble is mapped POSITIONALLY through speed_for_level(), against the COMMAND
// band — a 2-speed fan's top level is HIGH (0xB_), never MED (0xA_).
::quietcool::FanState timer_command_from_intent(TimerSelection requested, bool fan_on,
                                                int level,
                                                std::uint8_t command_speed_count);

}  // namespace esphome::quietcool
