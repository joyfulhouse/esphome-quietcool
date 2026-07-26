// Feedback-mapping tests for the fan adapter's confirmed-authority -> Home
// Assistant display translation (issue #18).
//
// This is the reporting-side mirror of the #15 command-mapping gap. Until now
// QuietCoolFan::publish_authority was linked into no test binary (it derives
// from esphome::fan::Fan, whose surface is too large to stub), so `state =
// confirmed->state.is_on()` could be inverted — showing a running fan as Off, or
// a stopped fan as On — with the whole suite green. Less severe than #15 (a
// wrong display cannot start a fan, so no backdraft exposure), but the same
// method also derives supported_speed_count from confirmed capability, and that
// value feeds the command path's clamp band.
//
// The mapping is now the pure free function authority_to_feedback(); these tests
// drive it directly. Inputs are OBSERVED FanStates (confirmed states always
// are), encoded raw as: bits 7-6 capability, bits 5-4 speed (1..3), bits 3-0
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

// The speed-count field tracks only a capability in the fan's real 1..3 band; an
// Unknown (0) capability must leave it unset, so supported_speed_count — and the
// command-path clamp ceiling it drives — can never be pulled to 0. This is the
// producer-side answer to the #19 degenerate case: report_capability() can emit
// Unknown, and the mapping is what filters it.
QC_TEST("fan_feedback", "only an in-band capability sets the speed count") {
  // capability Three -> 3; capability One -> 1.
  QC_CHECK_EQ(authority_to_feedback(observed(0xFF), 3).supported_speed_count,
              std::optional<std::uint8_t>(3));
  QC_CHECK_EQ(authority_to_feedback(observed(0x5F), 3).supported_speed_count,
              std::optional<std::uint8_t>(1));
  // capability Unknown (0) -> unset.
  QC_CHECK(
      !authority_to_feedback(observed(0x2F), 3).supported_speed_count.has_value());
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
  // running-high report, byte-identical to the remote's HIGH command.
  const auto high = authority_to_feedback(observed(0xBF), 3);
  QC_CHECK_EQ(high.supported_speed_count, std::optional<std::uint8_t>(2));
  QC_CHECK_EQ(high.speed, std::optional<int>(2));

  // The report's own capability wins over the caller's stale count (passed 3
  // above); when the report carries none, the caller's count is the band.
  // 0x2F: capability Unknown, speed Medium, Continuous.
  QC_CHECK_EQ(authority_to_feedback(observed(0x2F), 2).speed,
              std::optional<int>(2));

  // 0x9F observed: capability Two, speed Low — bottom of the band is level 1.
  QC_CHECK_EQ(authority_to_feedback(observed(0x9F), 3).speed,
              std::optional<int>(1));
}

// The two mappings must be inverses over every real band, or a user's selected
// level drifts after confirmation. Drives level -> speed_for_level -> a
// same-capability observed report -> authority_to_feedback -> level.
// Mutation: break either direction's band rule and some (count, level) pair
// fails the roundtrip.
QC_TEST("fan_feedback", "command and feedback mappings roundtrip on every band") {
  for (std::uint8_t count = 1; count <= 3; ++count) {
    for (int level = 1; level <= count; ++level) {
      const auto speed = speed_for_level(level, count);
      const std::uint8_t raw = static_cast<std::uint8_t>(
          (count << 6U) | (static_cast<std::uint8_t>(speed) << 4U) | 0x0FU);
      const auto feedback = authority_to_feedback(observed(raw), count);
      QC_CHECK_EQ(feedback.speed, std::optional<int>(level));
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
  // A restored capability narrows the band with no confirmed state at all.
  QC_CHECK_EQ(
      authority_speed_count(restored_snapshot(::quietcool::SpeedCapability::Two),
                            3),
      2);
  // No capability: keep the entity's current count (first ever boot).
  QC_CHECK_EQ(authority_speed_count(restored_snapshot(std::nullopt), 3), 3);
  // Out-of-band values cannot be produced by the core; defence in depth keeps
  // the function total anyway.
  QC_CHECK_EQ(authority_speed_count(
                  restored_snapshot(static_cast<::quietcool::SpeedCapability>(7)),
                  3),
              3);
}

// The #31 acceptance property end to end at the mapping layer: immediately
// after a reboot that restored capability Two, a Home Assistant level-2
// command must transmit HIGH (0xBF), not MED (0xAF, which stops the fan).
QC_TEST("fan_feedback", "level 2 maps to HIGH right after a capability-restored boot") {
  const auto count =
      authority_speed_count(restored_snapshot(::quietcool::SpeedCapability::Two),
                            3);
  const auto command = fan_command_from_intent(true, 2, count);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xBF);
}

}  // namespace
}  // namespace esphome::quietcool
