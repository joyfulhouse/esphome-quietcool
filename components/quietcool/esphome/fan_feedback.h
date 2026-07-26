#pragma once

#include "quietcool/core/fan_state.h"

#include <cstdint>
#include <optional>

namespace esphome::quietcool {

// The reporting-side mirror of fan_command_from_intent (issue #18): translates a
// CONFIRMED FanState (the value the publication gate has already accepted) into
// what a Home Assistant fan entity should display. Kept free of any ESPHome
// dependency so the mapping is unit-testable directly — an inversion of `on`
// here reports a running fan as Off, or a stopped fan as On.
//
// `speed` and `supported_speed_count` are optional because the entity only
// overwrites its current values when the confirmed state actually carries them:
// a report with no speed, or with no in-band capability, must leave those fields
// untouched. `on` is always defined — is_on() is total.
struct FanFeedback final {
  bool on{false};
  std::optional<int> speed;
  std::optional<std::uint8_t> supported_speed_count;
};

// `speed` is a Home Assistant LEVEL (a position in the 1..count band), not the
// wire nibble: level = min(nibble, count), the inverse of speed_for_level. The
// mapping uses the report's own capability when it carries one in 1..3, else
// `current_supported_speed_count` — the entity's value at call time — so a
// report that narrows the band and carries a speed stays self-consistent.
FanFeedback authority_to_feedback(const ::quietcool::FanState& confirmed,
                                  std::uint8_t current_supported_speed_count);

}  // namespace esphome::quietcool
