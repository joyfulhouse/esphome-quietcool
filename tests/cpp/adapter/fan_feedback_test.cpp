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
// The mapping is now two pure free functions — authority_speed_count() for the
// band and authority_to_feedback() for what to display inside it; these tests
// drive them directly. Inputs to the latter are OBSERVED FanStates (confirmed
// states always are), encoded raw as: bits 7-6 capability, bits 5-4 speed
// (1..3), bits 3-0 duration.

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

// Issue #31: the entity's speed count is seeded from the snapshot's sticky
// speed_capability BEFORE the publication gate, so it is right at the
// restore-time publication — the one the gate swallows for state purposes.
::quietcool::AuthoritySnapshot restored_snapshot(
    std::optional<::quietcool::SpeedCapability> capability) {
  return {::quietcool::UnknownStateAuthority{
              ::quietcool::AuthorityLossReason::RestoredUnverified, 0,
              std::nullopt, std::nullopt},
          ::quietcool::UnknownTimerAuthority{
              ::quietcool::TimerLossReason::RestoredUnverified, 0},
          std::nullopt,
          capability,
          std::nullopt,
          0};
}

QC_TEST("fan_feedback", "authority speed count seeds from the sticky capability") {
  // A restored capability narrows — or widens — the band with no confirmed
  // state at all.
  QC_CHECK_EQ(
      authority_speed_count(restored_snapshot(::quietcool::SpeedCapability::Two)),
      2);
  QC_CHECK_EQ(authority_speed_count(
                  restored_snapshot(::quietcool::SpeedCapability::Three)),
              3);
  QC_CHECK_EQ(
      authority_speed_count(restored_snapshot(::quietcool::SpeedCapability::One)),
      1);
  // No capability: the UNKNOWN-CAPABILITY band, never a previously cached
  // count. The function is pure in the snapshot precisely so that after Forget
  // or after learning a DIFFERENT fan (both clear the sticky capability) the
  // entity cannot keep the old fan's band (issue #31 review).
  QC_CHECK_EQ(authority_speed_count(restored_snapshot(std::nullopt)),
              kUnknownCapabilitySpeedCount);
  // Out-of-band values cannot be produced by the core; defence in depth keeps
  // the function total anyway.
  QC_CHECK_EQ(authority_speed_count(restored_snapshot(
                  static_cast<::quietcool::SpeedCapability>(7))),
              kUnknownCapabilitySpeedCount);
}

// The #31 acceptance property end to end at the mapping layer: immediately
// after a reboot that restored capability Two, a Home Assistant level-2
// command must transmit HIGH (0xBF), not MED (0xAF, which stops the fan).
QC_TEST("fan_feedback", "level 2 maps to HIGH right after a capability-restored boot") {
  const auto count =
      authority_speed_count(restored_snapshot(::quietcool::SpeedCapability::Two));
  const auto command = fan_command_from_intent(true, 2, count);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xBF);
}

// Issue #31 review (opus-xhigh): the rebinding case, which is where a PURE
// authority_speed_count would otherwise open a fresh route to #30's stopped
// fan. Home Assistant caches supported_speed_count from ListEntities and
// refreshes it only on reconnect, so after a mid-session Learn of a different
// fan its band and the device's disagree, and the device has no way to re-list
// or to re-query (radio_ready_seen_ has latched, and an ON request answered by
// a marker-bearing mismatch never promotes). Whatever band Home Assistant
// believes, its press arrives as a level in 1..3 — and against the
// unknown-capability band NONE of them can form MED, the one speed a 2-speed
// fan lacks. Mutation: widen kUnknownCapabilitySpeedCount to 3 and the level-2
// leg transmits 0xAF.
QC_TEST("fan_feedback", "a rebinding publication can never transmit MED") {
  const auto count = authority_speed_count(restored_snapshot(std::nullopt));
  QC_CHECK_EQ(count, kUnknownCapabilitySpeedCount);
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
// CONFIRMED three speeds. Mutation: any widening of the unknown band, or an
// identity level->nibble mapping, produces a Medium here without a Three.
QC_TEST("fan_feedback", "MED is formed only for a confirmed three-speed fan") {
  const std::optional<::quietcool::SpeedCapability> capabilities[] = {
      std::nullopt, ::quietcool::SpeedCapability::One,
      ::quietcool::SpeedCapability::Two, ::quietcool::SpeedCapability::Three};
  bool medium_seen = false;
  for (const auto capability : capabilities) {
    const auto count = authority_speed_count(restored_snapshot(capability));
    for (int level = 0; level <= 5; ++level) {
      for (const bool on : {false, true}) {
        const auto command = fan_command_from_intent(on, level, count);
        const bool medium =
            command.speed() ==
            std::optional<::quietcool::Speed>(::quietcool::Speed::Medium);
        medium_seen = medium_seen || medium;
        QC_CHECK(!medium ||
                 capability == std::optional<::quietcool::SpeedCapability>(
                                   ::quietcool::SpeedCapability::Three));
      }
    }
  }
  // Non-vacuity: a confirmed 3-speed fan really can be commanded to MED, so the
  // implication above is not satisfied by never forming Medium at all.
  QC_CHECK(medium_seen);
}

}  // namespace
}  // namespace esphome::quietcool
