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
#include <string>

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

// ---------------------------------------------------------------------------
// Option strings and authority feedback.
//
// These live here, not in quietcool_timer_select.cpp, because that file derives
// from select::Select and is excluded from this test binary — the same reason
// fan_command_from_intent was extracted in issue #15. Logic welded into an
// unlinkable entity file can be inverted with the whole suite still green.
// ---------------------------------------------------------------------------

QC_TEST("timer_command", "option strings round trip through selection") {
  for (const auto* option : kTimerOptions) {
    const auto selection = selection_for_option(option);
    QC_CHECK(selection.has_value());
    QC_CHECK(std::string(option_for_selection(selection.value())) == std::string(option));
  }
}

QC_TEST("timer_command", "none is the continuous option") {
  // "None" means no TIMER, which on the wire is Continuous — the fan runs until
  // stopped. It does NOT mean Duration::Off, which would stop the fan.
  QC_CHECK_EQ(selection_for_option("None").value(), TimerSelection::Continuous);
  QC_CHECK(std::string(option_for_selection(TimerSelection::Continuous)) == "None");
}

QC_TEST("timer_command", "an unknown option is refused not guessed") {
  // A refusal must transmit nothing. Guessing a duration here would put an
  // energizing command on the air that the user never chose.
  QC_CHECK(!selection_for_option("3 hours").has_value());
  QC_CHECK(!selection_for_option("").has_value());
  QC_CHECK(!selection_for_option("none").has_value());
}

QC_TEST("timer_command", "unknown timer authority publishes nothing") {
  // Nothing confirmed means nothing to publish. Publishing "None" here would
  // assert a fact about the fan that no evidence supports.
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::UnknownTimerAuthority{};
  QC_CHECK(!timer_option_for_authority(snapshot).has_value());
}

QC_TEST("timer_command", "a programmed timer publishes its duration") {
  ::quietcool::AuthoritySnapshot snapshot{};
  ::quietcool::ProgrammedDurationAuthority programmed{};
  programmed.duration = ::quietcool::Duration::Hours4;
  snapshot.timer = programmed;
  QC_CHECK(std::string(timer_option_for_authority(snapshot).value()) == "4 hours");
}

QC_TEST("timer_command", "a locally anchored timer publishes its duration") {
  // Aggregate-initialised in full: TransactionId/AttemptNumber are opaque ids
  // with no default constructor, so `{}` does not compile here.
  ::quietcool::AuthoritySnapshot snapshot{};
  const ::quietcool::LocallyAnchoredTimerAuthority anchored{
      ::quietcool::Duration::Hours12,
      ::quietcool::TransactionId(1),
      ::quietcool::AttemptNumber(1),
      0,
      0,
      ::quietcool::EvidenceSource::PostCommandConsensus};
  snapshot.timer = anchored;
  QC_CHECK(std::string(timer_option_for_authority(snapshot).value()) == "12 hours");
}

QC_TEST("timer_command", "no active timer publishes none") {
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::NoActiveTimerAuthority{};
  QC_CHECK(std::string(timer_option_for_authority(snapshot).value()) == "None");
}

QC_TEST("timer_command", "a confirmed off duration never publishes a stop") {
  // Duration::Off should not reach a timer authority, but if it ever does the
  // select must fall back to "None" — the option list has no way to express a
  // stopped fan, and inventing one would put Off back into the timer path.
  ::quietcool::AuthoritySnapshot snapshot{};
  ::quietcool::ProgrammedDurationAuthority programmed{};
  programmed.duration = ::quietcool::Duration::Off;
  snapshot.timer = programmed;
  QC_CHECK(std::string(timer_option_for_authority(snapshot).value()) == "None");
}

}  // namespace
}  // namespace esphome::quietcool
