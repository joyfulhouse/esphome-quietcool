// Timer actuation mapping: TimerSelection -> FanState command byte.
//
// A timer command is speed|duration in ONE byte, so it is an energizing
// command and it carries a speed nibble like any other. Two failure modes are
// guarded here: expressing Duration::Off (which would STOP a fan the user
// asked to run for N hours), and mapping speed by identity (which on a
// 2-speed fan transmits MED, a speed it does not have — issue #30).

#include "quietcool/esphome/timer_command.h"

#include "quietcool/core/fan_state.h"

#include "support/test.h"

#include <cstdint>

namespace esphome::quietcool {
namespace {

using ::quietcool::Duration;
using ::quietcool::FanState;
using ::quietcool::Speed;

constexpr std::uint8_t kTwoSpeeds = 2;
constexpr std::uint8_t kThreeSpeeds = 3;

QC_TEST("timer_command", "timer selection maps to its duration") {
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Continuous), Duration::Continuous);
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Hours1), Duration::Hours1);
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Hours2), Duration::Hours2);
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Hours4), Duration::Hours4);
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Hours8), Duration::Hours8);
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Hours12), Duration::Hours12);
}

QC_TEST("timer_command", "no selection can produce a stop command") {
  // Every selection must leave the fan running. Duration::Off is the stop
  // command; it must be unreachable from the timer path.
  const TimerSelection all[] = {
      TimerSelection::Continuous, TimerSelection::Hours1, TimerSelection::Hours2,
      TimerSelection::Hours4,     TimerSelection::Hours8, TimerSelection::Hours12};
  for (const auto selection : all) {
    const auto command = timer_command_from_intent(selection, true, 1, kThreeSpeeds);
    QC_CHECK(command.is_on());
    QC_CHECK(command.duration() != Duration::Off);
  }
}

QC_TEST("timer_command", "running fan keeps its speed and takes the duration") {
  // High + 1 hour == 0xB1, per the legacy byte table.
  const auto command = timer_command_from_intent(TimerSelection::Hours1, true, 3, kThreeSpeeds);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xB1);
  QC_CHECK_EQ(command.speed().value(), Speed::High);
  QC_CHECK_EQ(command.duration(), Duration::Hours1);
}

QC_TEST("timer_command", "stopped fan defaults to low and starts") {
  // Legacy behavior, deliberately retained: a timer set on a stopped fan
  // STARTS it at LOW. Low + 2 hours == 0x92.
  const auto command = timer_command_from_intent(TimerSelection::Hours2, false, 3, kThreeSpeeds);
  QC_CHECK_EQ(command.outbound_command_byte(), 0x92);
  QC_CHECK_EQ(command.speed().value(), Speed::Low);
  QC_CHECK(command.is_on());
}

QC_TEST("timer_command", "two speed top level takes high never medium") {
  // The band is positional: level 2 of 2 is HIGH (0xB_), never MED (0xA_).
  // An identity cast here re-creates issue #30 through the timer path.
  const auto command = timer_command_from_intent(TimerSelection::Hours4, true, 2, kTwoSpeeds);
  QC_CHECK_EQ(command.speed().value(), Speed::High);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xB4);
}

QC_TEST("timer_command", "continuous selection matches the fan entity's on command") {
  // Selecting "None" (Continuous) must produce exactly what turning the fan on
  // produces, or the two controls would disagree about what "running" means.
  const auto command = timer_command_from_intent(TimerSelection::Continuous, true, 3, kThreeSpeeds);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xBF);
}

}  // namespace
}  // namespace esphome::quietcool
