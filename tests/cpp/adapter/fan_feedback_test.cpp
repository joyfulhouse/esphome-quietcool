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
  const auto running = authority_to_feedback(observed(0xFF));
  QC_CHECK(running.on);
  // Off (dur 0, speed nibble 0), capability Three -> stopped.
  const auto stopped = authority_to_feedback(observed(0xC0));
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
    const auto feedback = authority_to_feedback(observed(c.raw));
    QC_CHECK(feedback.speed.has_value());
    QC_CHECK_EQ(*feedback.speed, c.level);
  }
  // Stopped state has no speed nibble -> speed unset.
  QC_CHECK(!authority_to_feedback(observed(0xC0)).speed.has_value());
}

// The speed-count field tracks only a capability in the fan's real 1..3 band; an
// Unknown (0) capability must leave it unset, so supported_speed_count — and the
// command-path clamp ceiling it drives — can never be pulled to 0. This is the
// producer-side answer to the #19 degenerate case: report_capability() can emit
// Unknown, and the mapping is what filters it.
QC_TEST("fan_feedback", "only an in-band capability sets the speed count") {
  // capability Three -> 3; capability One -> 1.
  QC_CHECK_EQ(authority_to_feedback(observed(0xFF)).supported_speed_count,
              std::optional<std::uint8_t>(3));
  QC_CHECK_EQ(authority_to_feedback(observed(0x5F)).supported_speed_count,
              std::optional<std::uint8_t>(1));
  // capability Unknown (0) -> unset.
  QC_CHECK(
      !authority_to_feedback(observed(0x2F)).supported_speed_count.has_value());
}

}  // namespace
}  // namespace esphome::quietcool
