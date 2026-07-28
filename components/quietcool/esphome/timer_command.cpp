#include "timer_command.h"

#include "fan_command.h"
#include "fan_feedback.h"

namespace esphome::quietcool {

::quietcool::Duration duration_for_selection(TimerSelection selection) {
  switch (selection) {
    case TimerSelection::Hours1:  return ::quietcool::Duration::Hours1;
    case TimerSelection::Hours2:  return ::quietcool::Duration::Hours2;
    case TimerSelection::Hours4:  return ::quietcool::Duration::Hours4;
    case TimerSelection::Hours8:  return ::quietcool::Duration::Hours8;
    case TimerSelection::Hours12: return ::quietcool::Duration::Hours12;
    case TimerSelection::Continuous: break;
  }
  // Continuous is the default rather than a case so that adding a selection
  // without extending this switch keeps the fan RUNNING. Failing toward
  // Continuous is the safe direction; failing toward Off would stop the fan.
  return ::quietcool::Duration::Continuous;
}

::quietcool::FanState timer_command_from_intent(TimerSelection requested, bool fan_on,
                                                int level,
                                                std::uint8_t command_speed_count) {
  const auto speed = fan_on ? speed_for_level(level, command_speed_count)
                            : ::quietcool::Speed::Low;
  return ::quietcool::FanState::command(speed, duration_for_selection(requested));
}

namespace {

// The option index and the TimerSelection enumerator are the same ordinal, so
// the two directions cannot drift apart as long as both go through here.
constexpr TimerSelection kSelectionByIndex[] = {
    TimerSelection::Continuous, TimerSelection::Hours1, TimerSelection::Hours2,
    TimerSelection::Hours4,     TimerSelection::Hours8, TimerSelection::Hours12};

static_assert(sizeof(kSelectionByIndex) / sizeof(kSelectionByIndex[0]) ==
                  sizeof(kTimerOptions) / sizeof(kTimerOptions[0]),
              "every option string must have exactly one selection");

// The selection for a wire duration. Off degrades to Continuous ("None")
// rather than gaining an option of its own: the select's vocabulary has no
// stop, by design.
TimerSelection selection_for_duration(::quietcool::Duration duration) {
  switch (duration) {
    case ::quietcool::Duration::Hours1:  return TimerSelection::Hours1;
    case ::quietcool::Duration::Hours2:  return TimerSelection::Hours2;
    case ::quietcool::Duration::Hours4:  return TimerSelection::Hours4;
    case ::quietcool::Duration::Hours8:  return TimerSelection::Hours8;
    case ::quietcool::Duration::Hours12: return TimerSelection::Hours12;
    case ::quietcool::Duration::Continuous:
    case ::quietcool::Duration::Off:
      break;
  }
  return TimerSelection::Continuous;
}

// The option for a wire duration, DERIVED from kTimerOptions via the selection
// ordinal rather than restated as string literals. A second list of literals
// here is the round-1 opus finding: rename an option in both select.py and
// kTimerOptions -- the exact edit the codegen-drift test blesses -- and the
// stale literal would still be published, which Select::publish_state refuses
// (no such option), leaving the entity permanently stateless in Home Assistant
// while it keeps transmitting.
const char* option_for_duration(::quietcool::Duration duration) {
  return option_for_selection(selection_for_duration(duration));
}

}  // namespace

std::optional<TimerSelection> selection_for_option(const std::string& option) {
  for (std::size_t index = 0;
       index < sizeof(kTimerOptions) / sizeof(kTimerOptions[0]); ++index) {
    if (option == kTimerOptions[index]) return kSelectionByIndex[index];
  }
  return std::nullopt;
}

const char* option_for_selection(TimerSelection selection) {
  return kTimerOptions[static_cast<std::size_t>(selection)];
}

::quietcool::FanState timer_command_from_confirmed(
    const std::optional<::quietcool::FanState>& confirmed,
    std::optional<::quietcool::SpeedCapability> capability,
    TimerSelection requested) {
  // The band pair is asymmetric on purpose: the confirmed level is read against
  // the ENTITY band (the count the level was published in), while the outgoing
  // nibble is formed against the COMMAND band, which is 2 while capability is
  // unknown and therefore structurally cannot form MED (issues #30, #31). A
  // stale confirmed MED under an unknown capability clamps to HIGH -- one press
  // in the safe direction, never a stop. Do not "simplify" this to one band.
  const auto feedback =
      confirmed ? authority_to_feedback(*confirmed, entity_speed_count(capability))
                : FanFeedback{};
  return timer_command_from_intent(requested, feedback.on,
                                   feedback.speed.value_or(1),
                                   command_speed_count(capability));
}

std::optional<const char*> timer_option_for_authority(
    const ::quietcool::AuthoritySnapshot& authority) {
  if (const auto* anchored =
          std::get_if<::quietcool::LocallyAnchoredTimerAuthority>(&authority.timer))
    return option_for_duration(anchored->duration);
  if (const auto* programmed =
          std::get_if<::quietcool::ProgrammedDurationAuthority>(&authority.timer))
    return option_for_duration(programmed->duration);
  if (std::get_if<::quietcool::NoActiveTimerAuthority>(&authority.timer))
    return "None";
  // UnknownTimerAuthority: publish nothing. The entity keeps its last confirmed
  // value rather than claiming a fact no evidence supports.
  return std::nullopt;
}

}  // namespace esphome::quietcool
