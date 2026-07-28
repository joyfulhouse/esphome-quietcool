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

}  // namespace esphome::quietcool
