// Actuation-mapping tests for the fan adapter's HA-intent -> FanState translation.
//
// This is the single most safety-critical translation in the component and, until
// issue #15, was linked into no test binary: quietcool_fan.cpp derives from
// esphome::fan::Fan, whose surface is too large to stub, so the on/off -> Duration
// mapping welded inside its control() method could be inverted — turning a Home
// Assistant "turn on" into a fan that shuts OFF — with the entire suite staying
// green. In a whole-house fan in an occupied house that is a backdraft hazard with
// combustion appliances, not a display glitch.
//
// The mapping is now a pure free function, fan_command_from_intent(), with no
// ESPHome dependency. These tests drive it directly.

#include "quietcool/esphome/fan_command.h"

#include "quietcool/core/fan_state.h"

#include "support/test.h"

#include <cstdint>
#include <optional>

namespace esphome::quietcool {
namespace {

using ::quietcool::Duration;
using ::quietcool::FanState;
using ::quietcool::Speed;

constexpr std::uint8_t kThreeSpeeds = 3;
constexpr std::uint8_t kTwoSpeeds = 2;

// Acceptance criterion for #15: ON must command a running fan and OFF must
// command a stopped one. Inverting the on/off -> Duration mapping is the exact
// mutation that passed the whole suite before this test existed.
QC_TEST("fan_command", "ON commands a running fan, OFF commands a stopped fan") {
  const auto on = fan_command_from_intent(true, 2, kThreeSpeeds);
  QC_CHECK(on.is_on());
  QC_CHECK_EQ(on.duration(), Duration::Continuous);

  const auto off = fan_command_from_intent(false, 2, kThreeSpeeds);
  QC_CHECK(!off.is_on());
  QC_CHECK_EQ(off.duration(), Duration::Off);
}

// Every speed the adapter exposes must map to its intended core Speed, with no
// gaps and no silent defaulting. Mutation: collapse two levels to one value.
QC_TEST("fan_command", "each HA speed maps to its intended core Speed") {
  struct Case final {
    int level;
    Speed expected;
  };
  constexpr Case cases[] = {
      {1, Speed::Low}, {2, Speed::Medium}, {3, Speed::High}};
  for (const auto& c : cases) {
    const std::optional<Speed> mapped =
        fan_command_from_intent(true, c.level, kThreeSpeeds).speed();
    QC_CHECK(mapped.has_value());
    QC_CHECK_EQ(*mapped, c.expected);
  }
}

// Out-of-range input must clamp into [1, supported_speed_count], never form an
// undefined speed nibble. Mutation: drop either clamp bound; the out-of-range
// input then casts to an invalid Speed and speed() reads back empty or wrong.
QC_TEST("fan_command", "out-of-range speed clamps into the supported band") {
  // Below the floor clamps up to Low.
  const auto too_low = fan_command_from_intent(true, 0, kThreeSpeeds);
  QC_CHECK(too_low.speed().has_value());
  QC_CHECK_EQ(*too_low.speed(), Speed::Low);
  QC_CHECK_EQ(too_low, FanState::command(Speed::Low, Duration::Continuous));

  // Just above the ceiling clamps down to the top supported speed.
  const auto too_high = fan_command_from_intent(true, 4, kThreeSpeeds);
  QC_CHECK(too_high.speed().has_value());
  QC_CHECK_EQ(*too_high.speed(), Speed::High);

  // The ceiling tracks supported_speed_count: a 3 on a 2-speed fan is Medium.
  const auto over_two = fan_command_from_intent(true, 3, kTwoSpeeds);
  QC_CHECK(over_two.speed().has_value());
  QC_CHECK_EQ(*over_two.speed(), Speed::Medium);
}

// #19: the clamp must be total. With supported_speed_count == 0 the pre-fix code
// returned 0, which casts to an undefined Speed nibble; the safety currently
// relied on a caller guard in a different, untested file (publish_authority).
// Mutation: drop the ceiling floor in clamp_fan_speed -> clamp_fan_speed(>=1, 0)
// returns 0, which speed_from_value rejects, failing this test.
QC_TEST("fan_command", "clamp is total: a zero supported count still yields a valid Speed") {
  for (const int n : {-3, 0, 1, 2, 3, 4, 99}) {
    const int clamped = clamp_fan_speed(n, 0);
    QC_CHECK(::quietcool::speed_from_value(static_cast<std::uint8_t>(clamped))
                 .has_value());
  }
  // The whole translation stays safe too: a 0 count yields a running command
  // whose speed nibble is valid rather than undefined.
  QC_CHECK(fan_command_from_intent(true, 1, 0).speed().has_value());
}

}  // namespace
}  // namespace esphome::quietcool
