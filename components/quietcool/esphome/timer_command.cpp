#include "timer_command.h"

#include "fan_command.h"

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

// The option for a wire duration. Off degrades to "None" rather than gaining an
// option of its own: the select's vocabulary has no stop, by design.
const char* option_for_duration(::quietcool::Duration duration) {
  switch (duration) {
    case ::quietcool::Duration::Hours1:  return "1 hour";
    case ::quietcool::Duration::Hours2:  return "2 hours";
    case ::quietcool::Duration::Hours4:  return "4 hours";
    case ::quietcool::Duration::Hours8:  return "8 hours";
    case ::quietcool::Duration::Hours12: return "12 hours";
    case ::quietcool::Duration::Continuous:
    case ::quietcool::Duration::Off:
      break;
  }
  return "None";
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
