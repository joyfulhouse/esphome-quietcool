// Feedback-mapping tests for the fan adapter's confirmed-authority -> Home
// Assistant display translation (issue #18).
//
// This is the reporting-side mirror of the #15 command-mapping gap. Until now
// QuietCoolFan::publish_authority was linked into no test binary (it derives
// from esphome::fan::Fan, whose surface is too large to stub), so `state =
// confirmed->state.is_on()` could be inverted — showing a running fan as Off, or
// a stopped fan as On — with the whole suite green. Less severe than #15 (a
// wrong display cannot start a fan, so no backdraft exposure), but the same
// method also sets the entity's speed count, and that value feeds the command
// path's clamp band.
//
// The mapping is now pure free functions — entity_speed_count() and
// command_speed_count() for the two bands, authority_to_feedback() for what to
// display inside the entity one — plus FanSpeedBands, which holds the entity
// band's monotonic latch. These tests drive them directly. Inputs to
// authority_to_feedback are OBSERVED FanStates (confirmed states always are),
// encoded raw as: bits 7-6 capability, bits 5-4 speed (1..3), bits 3-0
// duration.

#include "quietcool/esphome/fan_feedback.h"

#include "quietcool/esphome/fan_command.h"

#include "quietcool/core/fan_state.h"

#include "support/test.h"

#include <cstdint>
#include <optional>

namespace esphome::quietcool {
namespace {

using ::quietcool::FanState;

FanState observed(std::uint8_t raw) {
  const auto result = FanState::observed(raw);
  QC_CHECK(result);  // guards the raw encodings below, not the mapping
  return result.value();
}

// Acceptance criterion for #18: a confirmed RUNNING fan must report on, a
// confirmed STOPPED fan must report off. Inverting is_on() in the mapping is the
// exact mutation that passes the whole suite while this path is unlinked.
QC_TEST("fan_feedback", "running reports on, stopped reports off") {
  // High + Continuous (dur 15), capability Three -> running.
  const auto running = authority_to_feedback(observed(0xFF), 3);
  QC_CHECK(running.on);
  // Off (dur 0, speed nibble 0), capability Three -> stopped.
  const auto stopped = authority_to_feedback(observed(0xC0), 3);
  QC_CHECK(!stopped.on);
}

// Confirmed speed maps straight through; a report carrying no speed leaves the
// field unset so the entity keeps its last value rather than defaulting.
QC_TEST("fan_feedback", "confirmed speed maps through; absent speed stays unset") {
  struct Case final {
    std::uint8_t raw;
    int level;
  };
  // Running (dur 15, capability Three) at Low / Medium / High.
  constexpr Case cases[] = {{0xDF, 1}, {0xEF, 2}, {0xFF, 3}};
  for (const auto& c : cases) {
    const auto feedback = authority_to_feedback(observed(c.raw), 3);
    QC_CHECK(feedback.speed.has_value());
    QC_CHECK_EQ(*feedback.speed, c.level);
  }
  // Stopped state has no speed nibble -> speed unset.
  QC_CHECK(!authority_to_feedback(observed(0xC0), 3).speed.has_value());
}

// Issue #30, the reporting half. A 2-speed fan reports HIGH as wire nibble 3
// (its running-high report is 0xBF: capability Two, speed 3, Continuous), but
// its HA entity has only 2 levels. Publishing the raw nibble put level 3 into
// a 2-level entity — unrepresentable, so a remote HIGH press showed nothing.
// level = min(nibble, count) publishes it as level 2, the top of the band.
// Mutation: revert to publishing the raw nibble -> the first check fails on
// 3 != 2 while the 3-speed identity cases above stay green.
QC_TEST("fan_feedback", "2-speed fan: confirmed HIGH publishes as the top level") {
  // 0xBF observed: capability Two, speed High, Continuous — the fan's real
  // running-high report, byte-identical to the remote's HIGH command. The band
  // passed is the one the same snapshot publishes: 2, learned from this very
  // report by the core's promote().
  QC_CHECK_EQ(authority_to_feedback(observed(0xBF), 2).speed,
              std::optional<int>(2));

  // An unexpected MED report on a 2-speed fan folds to the top of the band
  // rather than dropping the update. 0x2F: capability Unknown, speed Medium.
  QC_CHECK_EQ(authority_to_feedback(observed(0x2F), 2).speed,
              std::optional<int>(2));

  // 0x9F observed: capability Two, speed Low — bottom of the band is level 1.
  QC_CHECK_EQ(authority_to_feedback(observed(0x9F), 2).speed,
              std::optional<int>(1));
}

// Issue #31 review (opus-xhigh and codex, same root cause): the published LEVEL
// is clamped by the entity's BAND and never by the confirmed report's own
// capability marker bits. Every command-shaped frame carries marker bits 10,
// which alias SpeedCapability::Two — and a fan's confirming report of our own
// HIGH command is byte-identical to that command. The core deliberately refuses
// to let such an ambiguous frame demote a fan already known to have three
// speeds, so honouring the report's markers here published level 2 against
// band 3: Home Assistant showed 2/3 (~66%) while the fan ran at HIGH, and the
// entity then reused that cached level for the next implicit-speed command and
// transmitted MED (0xAF) instead of HIGH.
// Mutation: re-derive the clamp band from confirmed.report_capability() and the
// first check fails on 2 != 3.
QC_TEST("fan_feedback", "the band clamps the level; the report's markers never do") {
  // 0xBF on a fan whose sticky capability is Three: the bridge's own HIGH
  // command byte, echoed back. Band and level must agree at the top.
  QC_CHECK_EQ(authority_to_feedback(observed(0xBF), 3).speed,
              std::optional<int>(3));
  // The downstream half of the codex trajectory: a later turn-on with no
  // explicit speed reuses the cached level against the same band.
  QC_CHECK_EQ(fan_command_from_intent(true, 3, 3).outbound_command_byte(), 0xBF);

  // The inverse direction is equally the band's call: a capability-Three report
  // may not publish level 3 into a 2-level entity (that unrepresentable level
  // is issue #30's disappearing update).
  QC_CHECK_EQ(authority_to_feedback(observed(0xFF), 2).speed,
              std::optional<int>(2));

  // Capability One markers (01) are ignored just the same.
  QC_CHECK_EQ(authority_to_feedback(observed(0x7F), 3).speed,
              std::optional<int>(3));
}

// Structural restatement of the same property: whatever the report's marker
// bits claim, the published level is always inside the published band, so Home
// Assistant can always represent it.
QC_TEST("fan_feedback", "the published level always lies inside the band") {
  for (std::uint8_t count = 1; count <= 3; ++count) {
    for (std::uint8_t capability = 0; capability <= 3; ++capability) {
      for (std::uint8_t nibble = 1; nibble <= 3; ++nibble) {
        const std::uint8_t raw = static_cast<std::uint8_t>(
            (capability << 6U) | (nibble << 4U) | 0x0FU);
        const auto feedback = authority_to_feedback(observed(raw), count);
        QC_CHECK(feedback.speed.has_value());
        QC_CHECK(*feedback.speed >= 1 && *feedback.speed <= count);
      }
    }
  }
}

// The two mappings must be inverses over every real band, or a user's selected
// level drifts after confirmation. Drives level -> speed_for_level -> an
// observed report -> authority_to_feedback -> level, once with the fan's
// genuine capability bits and once with the command-shaped marker bits (10)
// that a confirming echo carries: the roundtrip must not depend on them.
// Mutation: break either direction's band rule and some (count, level) pair
// fails the roundtrip.
QC_TEST("fan_feedback", "command and feedback mappings roundtrip on every band") {
  for (std::uint8_t count = 1; count <= 3; ++count) {
    for (int level = 1; level <= count; ++level) {
      const auto speed = speed_for_level(level, count);
      for (const std::uint8_t capability_bits : {count, std::uint8_t{2}}) {
        const std::uint8_t raw = static_cast<std::uint8_t>(
            (capability_bits << 6U) | (static_cast<std::uint8_t>(speed) << 4U) |
            0x0FU);
        const auto feedback = authority_to_feedback(observed(raw), count);
        QC_CHECK_EQ(feedback.speed, std::optional<int>(level));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Issue #31 and its review: the two bands.
//
// entity_speed_count() is what get_traits() lists and what a published level is
// rendered against; command_speed_count() is what an inbound Home Assistant
// level is mapped against on the way to the wire. Both are pure in the sticky
// capability, so the adapter caches no count of its own; FanSpeedBands adds the
// one piece of state they need — the latch that stops the listed band widening
// under a connection that cannot be re-listed.
// ---------------------------------------------------------------------------

using Capability = ::quietcool::SpeedCapability;
constexpr std::optional<Capability> kUnlearned{};

QC_TEST("fan_feedback", "both bands read the sticky capability, not a cached count") {
  // A restored or confirmed capability sets both bands with no confirmed state
  // at all — the restore-time publication the gate swallows.
  QC_CHECK_EQ(entity_speed_count(Capability::One), 1);
  QC_CHECK_EQ(entity_speed_count(Capability::Two), 2);
  QC_CHECK_EQ(entity_speed_count(Capability::Three), 3);
  QC_CHECK_EQ(command_speed_count(Capability::One), 1);
  QC_CHECK_EQ(command_speed_count(Capability::Two), 2);
  QC_CHECK_EQ(command_speed_count(Capability::Three), 3);

  // No capability: each band's own UNLEARNED default, never a previously cached
  // count. Both functions are pure in the capability precisely so that after
  // Forget or after learning a DIFFERENT fan (both clear the sticky capability)
  // the entity cannot keep the old fan's band (issue #31 review).
  QC_CHECK_EQ(entity_speed_count(kUnlearned), kUnlearnedEntitySpeedCount);
  QC_CHECK_EQ(command_speed_count(kUnlearned), kUnlearnedCommandSpeedCount);

  // Out-of-band values cannot be produced by the core; defence in depth keeps
  // both functions total anyway.
  QC_CHECK_EQ(entity_speed_count(static_cast<Capability>(7)),
              kUnlearnedEntitySpeedCount);
  QC_CHECK_EQ(command_speed_count(static_cast<Capability>(7)),
              kUnlearnedCommandSpeedCount);
}

// Issue #31 review (opus-xhigh), the finding itself. Home Assistant reads
// supported_speed_count once per API connection, at ListEntities, and caches it
// until it reconnects — the device cannot re-list. So the band it lists while
// nothing is known must be at least as wide as every band the fan can later
// prove, or learning WIDENS the entity: Home Assistant keeps sending levels
// from the narrower band it cached, its "100%" arrives as a middle level of the
// wider band and maps to MED, and HIGH becomes unreachable for the whole
// session (on a fan that turns out to have two speeds, MED stops it).
// Mutation: narrow kUnlearnedEntitySpeedCount to 2 and the Three leg fails.
QC_TEST("fan_feedback", "the unlearned entity band is at least every learnable band") {
  const auto unlearned = entity_speed_count(kUnlearned);
  for (const auto capability :
       {Capability::One, Capability::Two, Capability::Three}) {
    QC_CHECK(entity_speed_count(capability) <= unlearned);
  }
}

// The same property as a trajectory through FanSpeedBands, including the door
// the unlearned default alone does not close: an Unambiguous report overwrites
// the sticky capability outright, so a 3-speed fan first mis-learned as Two
// from its own echo self-heals Two -> Three. The listed band must not follow it
// back up. Mutation: drop the latch in FanSpeedBands::observe (assign instead
// of min) and the last check fails on 3 != 2.
QC_TEST("fan_feedback", "the listed band never widens, whatever order evidence arrives") {
  const std::optional<Capability> trajectories[][4] = {
      {kUnlearned, Capability::Three, Capability::Two, Capability::One},
      {kUnlearned, Capability::Two, Capability::Three, Capability::Three},
      {Capability::One, Capability::Three, kUnlearned, Capability::Two},
      {kUnlearned, kUnlearned, Capability::Three, kUnlearned},
  };
  for (const auto& trajectory : trajectories) {
    FanSpeedBands bands;
    std::uint8_t previous = bands.entity();
    QC_CHECK_EQ(previous, kUnlearnedEntitySpeedCount);
    for (const auto capability : trajectory) {
      bands.observe(capability);
      QC_CHECK(bands.entity() <= previous);
      previous = bands.entity();
    }
  }

  // The mis-learn trajectory spelled out, because it is the one a fresh 3-speed
  // install actually takes.
  FanSpeedBands bands;
  bands.observe(Capability::Two);  // echo-ranked mis-learn narrows the entity
  QC_CHECK_EQ(bands.entity(), 2);
  bands.observe(Capability::Three);  // an unambiguous report self-heals the core
  QC_CHECK_EQ(bands.entity(), 2);    // ...but Home Assistant still holds 2
}

// The command band is bounded by the latched entity band, which is what makes
// it no wider than the band Home Assistant is working in: the entity band only
// narrows, so the count listed at connect time is >= today's, and command <=
// today's entity band <= the cached one. Mutation: drop the bound in
// FanSpeedBands::observe and the self-healed leg reports command 3 against a
// listed band of 2 — Home Assistant's 100% would then transmit MED.
QC_TEST("fan_feedback", "the command band is never wider than the listed band") {
  const std::optional<Capability> capabilities[] = {
      kUnlearned, Capability::One, Capability::Two, Capability::Three};
  for (const auto first : capabilities) {
    for (const auto second : capabilities) {
      FanSpeedBands bands;
      bands.observe(first);
      QC_CHECK(bands.command() <= bands.entity());
      bands.observe(second);
      QC_CHECK(bands.command() <= bands.entity());
    }
  }

  // The self-heal, concretely: capability rises to Three but the listed band is
  // latched at 2, so an inbound level is still mapped against 2.
  FanSpeedBands bands;
  bands.observe(Capability::Two);
  bands.observe(Capability::Three);
  QC_CHECK_EQ(bands.command(), 2);

  // And the ordinary 3-speed install, where nothing was ever latched narrower:
  // MED becomes commandable as soon as the fan proves it has three speeds.
  FanSpeedBands fresh;
  fresh.observe(kUnlearned);
  QC_CHECK_EQ(fresh.command(), kUnlearnedCommandSpeedCount);
  fresh.observe(Capability::Three);
  QC_CHECK_EQ(fresh.entity(), 3);
  QC_CHECK_EQ(fresh.command(), 3);
}

// The acceptance property the two bands exist for, driven over every band Home
// Assistant may have cached (any entity band the device can list) and every
// capability the device may hold when the press arrives: the TOP of Home
// Assistant's band always transmits HIGH (0xBF), and the bottom always LOW.
// This is the check the reviewer's probe failed — with one collapsed band, a
// device that listed 2 and then learned Three answered its own 100% with MED.
QC_TEST("fan_feedback", "the top of Home Assistant's cached band always transmits HIGH") {
  const std::optional<Capability> capabilities[] = {
      kUnlearned, Capability::One, Capability::Two, Capability::Three};
  for (const auto listed_at_connect : capabilities) {
    for (const auto learned_later : capabilities) {
      FanSpeedBands bands;
      bands.observe(listed_at_connect);
      const int cached_top = bands.entity();  // what Home Assistant now renders
      bands.observe(learned_later);
      QC_CHECK_EQ(fan_command_from_intent(true, cached_top, bands.command())
                      .outbound_command_byte(),
                  0xBF);
      // ...and its bottom always LOW, except on a 1-level band where the single
      // level IS the top and takes the same top-of-band rule (speed_for_level's
      // recorded choice for a unit type that does not exist in the field).
      QC_CHECK_EQ(
          fan_command_from_intent(true, 1, bands.command()).outbound_command_byte(),
          bands.command() >= 2 ? 0x9F : 0xBF);
    }
  }
}

// The display half of the same divergence: a published level must stay
// renderable in the band Home Assistant cached. Widening broke this too — after
// learning Three a device that listed 2 published level 3 into a 2-level
// entity, which is issue #30's update that disappears.
QC_TEST("fan_feedback", "a published level always fits the band Home Assistant cached") {
  const std::optional<Capability> capabilities[] = {
      kUnlearned, Capability::One, Capability::Two, Capability::Three};
  for (const auto listed_at_connect : capabilities) {
    for (const auto learned_later : capabilities) {
      FanSpeedBands bands;
      bands.observe(listed_at_connect);
      const int cached_band = bands.entity();
      bands.observe(learned_later);
      for (std::uint8_t nibble = 1; nibble <= 3; ++nibble) {
        const auto raw =
            static_cast<std::uint8_t>(0xC0U | (nibble << 4U) | 0x0FU);
        const auto feedback =
            authority_to_feedback(observed(raw), bands.entity());
        QC_CHECK(feedback.speed.has_value());
        QC_CHECK(*feedback.speed <= cached_band);
      }
    }
  }
}

// The #31 acceptance property end to end at the mapping layer: immediately
// after a reboot that restored capability Two, a Home Assistant level-2
// command must transmit HIGH (0xBF), not MED (0xAF, which stops the fan).
QC_TEST("fan_feedback", "level 2 maps to HIGH right after a capability-restored boot") {
  FanSpeedBands bands;
  bands.observe(Capability::Two);
  QC_CHECK_EQ(bands.entity(), 2);
  const auto command = fan_command_from_intent(true, 2, bands.command());
  QC_CHECK_EQ(command.outbound_command_byte(), 0xBF);
}

// Issue #31 review (opus-xhigh): the rebinding case, which is where a band
// carried over from the previous fan would open a fresh route to #30's stopped
// fan. Home Assistant caches supported_speed_count from ListEntities and
// refreshes it only on reconnect, so after a mid-session Learn of a different
// fan its band and the device's disagree, and the device has no way to re-list
// or to re-query (radio_ready_seen_ has latched, and an ON request answered by
// a marker-bearing mismatch never promotes). Whatever band Home Assistant
// believes, its press arrives as a level in 1..3 — and against the unlearned
// command band NONE of them can form MED, the one speed a 2-speed fan lacks.
// Mutation: widen kUnlearnedCommandSpeedCount to 3 and the level-2 leg
// transmits 0xAF.
QC_TEST("fan_feedback", "a rebinding publication can never transmit MED") {
  // The previous fan taught Three; then the binding changes and the snapshot
  // comes back capability-less.
  FanSpeedBands bands;
  bands.observe(Capability::Three);
  bands.observe(kUnlearned);
  const auto count = bands.command();
  QC_CHECK_EQ(count, kUnlearnedCommandSpeedCount);
  QC_CHECK_EQ(fan_command_from_intent(true, 1, count).outbound_command_byte(),
              0x9F);
  QC_CHECK_EQ(fan_command_from_intent(true, 2, count).outbound_command_byte(),
              0xBF);
  QC_CHECK_EQ(fan_command_from_intent(true, 3, count).outbound_command_byte(),
              0xBF);
}

// The general form of that safety property, over every capability the snapshot
// can carry, every level Home Assistant can send against a stale cached band,
// and both intents: the MED nibble reaches the wire only once the fan has
// CONFIRMED three speeds. Mutation: any widening of the unlearned command band,
// or an identity level->nibble mapping, produces a Medium here without a Three.
QC_TEST("fan_feedback", "MED is formed only for a confirmed three-speed fan") {
  const std::optional<Capability> capabilities[] = {
      kUnlearned, Capability::One, Capability::Two, Capability::Three};
  bool medium_seen = false;
  for (const auto capability : capabilities) {
    const auto count = command_speed_count(capability);
    for (int level = 0; level <= 5; ++level) {
      for (const bool on : {false, true}) {
        const auto command = fan_command_from_intent(on, level, count);
        const bool medium =
            command.speed() ==
            std::optional<::quietcool::Speed>(::quietcool::Speed::Medium);
        medium_seen = medium_seen || medium;
        QC_CHECK(!medium ||
                 capability == std::optional<Capability>(Capability::Three));
      }
    }
  }
  // Non-vacuity: a confirmed 3-speed fan really can be commanded to MED, so the
  // implication above is not satisfied by never forming Medium at all.
  QC_CHECK(medium_seen);
}

}  // namespace
}  // namespace esphome::quietcool
