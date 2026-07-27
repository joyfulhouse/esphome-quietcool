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
// `speed` is optional because the entity only overwrites its current value when
// the confirmed state actually carries one: a report with no speed nibble must
// leave the level untouched. `on` is always defined — is_on() is total.
//
// There is deliberately no speed-count field here. The entity's band comes from
// entity_speed_count() alone; see authority_to_feedback below.
struct FanFeedback final {
  bool on{false};
  std::optional<int> speed;
};

// `speed` is a Home Assistant LEVEL (a position in the 1..count band), not the
// wire nibble: level = min(nibble, supported_speed_count), the inverse of
// speed_for_level.
//
// `supported_speed_count` is the ENTITY band — entity_speed_count() of the same
// sticky capability get_traits() lists — and is the ONLY band this mapping
// consults. It must be the entity band and never the command band: Home
// Assistant renders a published level against the count it was listed, so
// publishing against anything narrower misreports the percentage.
//
// The confirmed report's own capability marker bits are deliberately ignored
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

// ---------------------------------------------------------------------------
// TWO bands, because the adapter answers two different questions from the one
// sticky capability and their failure modes point in OPPOSITE directions
// (issue #31 review, opus-xhigh). Collapsing them onto a single count is what
// let the entity band widen mid-session. Both are pure functions of the
// capability, so the adapter caches the capability and never a count.
// ---------------------------------------------------------------------------

// The ENTITY band while the bound fan's capability is UNKNOWN: the first boot
// after an install or after an upgrade from a record with no capability flag,
// after Forget, and after learning a different fan.
//
// The WIDEST band the wire can express, and that is a compatibility property
// rather than a guess. Home Assistant reads supported_speed_count once per API
// connection, at ListEntities, and caches it until it reconnects; the device
// cannot re-list within a session. The entity band is therefore a contract that
// may only ever NARROW while a session lasts:
//   - narrowing stays consistent in both directions — Home Assistant keeps
//     sending levels from the wider band it cached and speed_for_level
//     saturates them at the top of the narrower one (against band 2, levels 2
//     and 3 are both High), while a published level is clamped into the
//     narrower band and so stays renderable;
//   - widening cannot be recovered from — Home Assistant keeps sending levels
//     from the NARROWER band it cached, so its top-of-band press (its "100%")
//     arrives as a MIDDLE level of the wider band and maps to MED, which stops
//     a 2-speed fan and, on a 3-speed fan, makes HIGH unreachable for the whole
//     session; and a published top-of-band level exceeds what Home Assistant
//     can render (issue #30's disappearing update).
// Starting from the widest makes every capability the fan can later confirm a
// narrowing, so the unrecoverable direction is unreachable by construction.
inline constexpr std::uint8_t kUnlearnedEntitySpeedCount = 3;

// The band an inbound Home Assistant LEVEL is mapped against on the way to the
// wire, while capability is UNKNOWN.
//
// Two, and that is a safety property. The wire nibbles are fixed (1=LOW, 2=MED,
// 3=HIGH) and a fan supports a SUBSET of them, but {LOW, HIGH} is supported by
// every unit — MED is the one speed a 2-speed fan lacks, and commanding it
// STOPS that fan (issue #30). speed_for_level only ever yields Medium when the
// band it maps against is 3, so pinning the unlearned command band at 2 makes
// "MED reaches the wire only once the fan has CONFIRMED three speeds" true by
// construction, for every level Home Assistant can send and whatever band it is
// working from. The cost is that the middle of a 3-speed fan's listed band runs
// it at HIGH until its first confirmed report — one press in the safe
// direction, versus a stopped fan in an occupied home.
//
// This band is deliberately allowed to be NARROWER than the entity band: it is
// never listed and never rendered, so nothing caches it and it may move in
// either direction the instant evidence arrives. Mapping a level from the wider
// entity band against it is exactly the narrowing case described above.
inline constexpr std::uint8_t kUnlearnedCommandSpeedCount = 2;

// The entity's supported speed count: what get_traits() lists to Home Assistant
// and the band authority_to_feedback publishes levels inside. Unknown
// capability yields kUnlearnedEntitySpeedCount, so learning can only narrow it.
//
// Takes the sticky capability itself rather than a snapshot, so the adapter
// stores exactly ONE value (AuthoritySnapshot::speed_capability, assigned
// wholesale) and derives both bands from it — two cached counts could drift out
// of step, one cannot. promote() folds every confirmed report's capability into
// the sticky slot and restore seeds it from NVS, so it is defined whenever a
// confirmed report's marker bits are — plus at restore time, before any RF
// round-trip, which is the window issue #31 closes. An absent capability (first
// ever boot, Forget, or a re-bind to a different fan) yields the unlearned
// band, never whatever the previous fan taught the entity.
std::uint8_t entity_speed_count(
    std::optional<::quietcool::SpeedCapability> capability);

// The band control() maps an inbound Home Assistant level against. Equal to
// entity_speed_count() once the capability is known; while it is unknown this
// is kUnlearnedCommandSpeedCount, which cannot form MED.
std::uint8_t command_speed_count(
    std::optional<::quietcool::SpeedCapability> capability);

// The whole of a fan entity's speed-band state: the pair of bands, and the
// latch that keeps the listed one honest across a Home Assistant connection.
//
// Choosing the widest unlearned band removes the ONE widening the fan takes on
// every install, but not every widening: an Unambiguous report overwrites the
// sticky capability outright (AuthorityStore::promote), so a 3-speed fan first
// mis-learned as Two from its own echo — the acknowledged cost of ranking
// echo-indistinguishable evidence — self-heals Two -> Three and would widen the
// entity from 2 back to 3 mid-session. Home Assistant cached 2 at ListEntities
// and cannot be told otherwise until it reconnects, so its "100%" would arrive
// as level 2, map against a band of 3 and transmit MED: HIGH unreachable for
// the rest of the session, and the fan STOPPED if the mis-learn was right.
//
// So the entity band is LATCHED monotonically non-increasing for the life of
// this object — one boot, and a boot always re-lists. Every capability the fan
// can prove may narrow it; nothing may widen it. A widening therefore costs a
// reboot, which is the same reboot that re-lists, and it is a widening only
// Home Assistant's own view was ever missing: the persisted capability makes
// the next boot list the fan's real band from the first ListEntities. (An API
// reconnect also re-lists, but nothing in the fan entity can observe one, so
// the latch stays conservative rather than guessing.)
//
// The command band is not latched (nothing caches it) but is BOUNDED by the
// latched entity band, which is what makes it <= whatever band Home Assistant
// holds: the entity band only ever narrows, so the band listed at connect time
// is >= today's, and command <= today's entity band <= the cached one. Mapping
// a level down into a narrower band is the direction speed_for_level saturates
// safely — the top of Home Assistant's band is always HIGH.
class FanSpeedBands final {
 public:
  // Folds a published snapshot's sticky capability in. Total: an absent
  // capability (first boot, Forget, a re-bind to a different fan) returns the
  // command band to the unlearned MED-free one, so the previous fan's band can
  // never keep aiming a level press.
  void observe(std::optional<::quietcool::SpeedCapability> capability);

  // What get_traits() lists and authority_to_feedback publishes levels inside.
  std::uint8_t entity() const { return entity_; }
  // What control() maps an inbound Home Assistant level against.
  std::uint8_t command() const { return command_; }

 private:
  std::uint8_t entity_{kUnlearnedEntitySpeedCount};
  std::uint8_t command_{kUnlearnedCommandSpeedCount};
};

}  // namespace esphome::quietcool
