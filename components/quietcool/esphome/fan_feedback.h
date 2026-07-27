#pragma once

#include "quietcool/core/authority_store.h"
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
// `speed` is optional because the entity only overwrites its current value when
// the confirmed state actually carries one: a report with no speed nibble must
// leave the level untouched. `on` is always defined — is_on() is total.
//
// There is deliberately no speed-count field here. The entity's band comes from
// authority_speed_count() alone; see authority_to_feedback below.
struct FanFeedback final {
  bool on{false};
  std::optional<int> speed;
};

// `speed` is a Home Assistant LEVEL (a position in the 1..count band), not the
// wire nibble: level = min(nibble, supported_speed_count), the inverse of
// speed_for_level.
//
// `supported_speed_count` is the entity's band — i.e. authority_speed_count()
// of the same snapshot — and is the ONLY band this mapping consults. The
// confirmed report's own capability marker bits are deliberately ignored
// (issue #31 review): an outbound command frame's marker bits (10) alias
// SpeedCapability::Two, so a frame that may be our own echo reads as a 2-speed
// report. The core already ranks that ambiguity and keeps the sticky
// capability, so honouring the raw marker here made the published LEVEL
// disagree with the published BAND — a confirmed HIGH on a known 3-speed fan
// published level 2 of 3 (Home Assistant showed 66% while the fan ran at HIGH),
// and the entity then reused that level for the next implicit-speed command and
// transmitted MED. One band, one source: level <= band holds by construction,
// which is also what keeps a confirmed HIGH representable in a 2-level entity
// (issue #30).
FanFeedback authority_to_feedback(const ::quietcool::FanState& confirmed,
                                  std::uint8_t supported_speed_count);

// The band the entity uses while the bound fan's capability is UNKNOWN: the
// first boot after an install or an upgrade from a record without the
// capability flag, after Forget, and after learning a different fan.
//
// Two, not the widest band, and that is a safety property rather than a guess
// (issue #31 review). The wire nibbles are fixed (1=LOW, 2=MED, 3=HIGH) and a
// fan supports a SUBSET of them, but {LOW, HIGH} is supported by every unit —
// MED is the one speed a 2-speed fan lacks, and commanding it STOPS that fan
// (issue #30). speed_for_level only ever yields Medium when the band is 3, so
// pinning the unknown band at 2 makes "MED is transmitted only once the fan has
// confirmed it has three speeds" true by construction, for every Home Assistant
// level and whatever stale band Home Assistant itself still has cached from
// ListEntities. The cost is that a 3-speed fan is offered LOW/HIGH only until
// its first confirmed report — a degraded band the next publication widens,
// versus a stopped fan in an occupied home.
inline constexpr std::uint8_t kUnknownCapabilitySpeedCount = 2;

// The entity's supported speed count for a published authority snapshot
// (issue #31). The snapshot's sticky speed_capability is the single source of
// truth: promote() folds every confirmed report's capability into it and
// restore seeds it from NVS, so it is defined whenever a confirmed report's
// marker bits are — plus at restore time, before any RF round-trip, which is
// the window this closes. Deliberately a PURE function of the snapshot — no
// "current count" input — so a stale entity cache cannot survive a capability
// clear: when the snapshot carries nothing (first ever boot, Forget, or a
// re-bind to a different fan) the count is kUnknownCapabilitySpeedCount, not
// whatever the previous fan taught the entity.
std::uint8_t authority_speed_count(
    const ::quietcool::AuthoritySnapshot& authority);

}  // namespace esphome::quietcool
