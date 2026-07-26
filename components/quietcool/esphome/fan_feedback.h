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

FanFeedback authority_to_feedback(const ::quietcool::FanState& confirmed);

}  // namespace esphome::quietcool
