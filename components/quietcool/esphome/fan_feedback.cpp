#include "fan_feedback.h"

namespace esphome::quietcool {

FanFeedback authority_to_feedback(const ::quietcool::FanState& confirmed) {
  FanFeedback feedback{};
  feedback.on = confirmed.is_on();
  if (const auto confirmed_speed = confirmed.speed())
    feedback.speed = static_cast<int>(*confirmed_speed);
  // Only a reported capability in the fan's real 1..3 band updates the speed
  // count. report_capability() also yields Unknown (0) when the top two bits are
  // clear; that is filtered here, so supported_speed_count never carries 0 and
  // the command-path clamp band stays in [1, 3]. This is a deliberate producer
  // guard, not just consumer defence (issue #19 floored the consumer too).
  if (const auto capability = confirmed.report_capability()) {
    const auto count = static_cast<std::uint8_t>(*capability);
    if (count >= 1 && count <= 3) feedback.supported_speed_count = count;
  }
  return feedback;
}

}  // namespace esphome::quietcool
