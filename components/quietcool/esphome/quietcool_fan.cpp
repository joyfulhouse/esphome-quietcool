#include "quietcool_fan.h"

#include "fan_command.h"
#include "fan_feedback.h"

#include "esphome/core/log.h"

namespace esphome::quietcool {
namespace {

constexpr char TAG[] = "quietcool.fan";

}  // namespace

void QuietCoolFan::setup() {
  if (speed == 0) speed = 1;
}

void QuietCoolFan::dump_config() {
  LOG_FAN("", "QuietCool Confirmed Fan", this);
}

fan::FanTraits QuietCoolFan::get_traits() {
  fan::FanTraits traits{};
  traits.set_speed(true);
  traits.set_supported_speed_count(supported_speed_count_);
  return traits;
}

void QuietCoolFan::control(const fan::FanCall& call) {
  if (controller_ == nullptr) {
    ESP_LOGE(TAG, "FanCall refused: controller is not configured");
    return;
  }
  const bool requested_state =
      call.get_state().has_value() ? *call.get_state() : state;
  const int requested_speed =
      call.get_speed().has_value() ? *call.get_speed() : speed;
  ESP_LOGD(TAG, "FanCall queued: state=%s speed=%d; awaiting confirmation",
           ONOFF(requested_state),
           clamp_fan_speed(requested_speed, supported_speed_count_));
  controller_->request_state(fan_command_from_intent(
      requested_state, requested_speed, supported_speed_count_));
}

void QuietCoolFan::publish_authority(
    const ::quietcool::AuthoritySnapshot& authority) {
  // Seed the speed count BEFORE the publication gate (issue #31): the
  // restore-time publication carries no confirmed state — the gate swallows
  // it — but its restored speed_capability must reach get_traits() before
  // Home Assistant can send a command, or a level-2 press on a 2-speed fan is
  // mapped against the compiled default of 3 and transmits MED, which stops
  // the fan (issue #30's failure, transiently). The snapshot's sticky
  // capability is the single source of truth here, and it strictly DOMINATES
  // the per-report FanFeedback::supported_speed_count rather than merely
  // matching it: promote() folds every confirmed report's capability into the
  // snapshot, so the snapshot is defined whenever the report's marker bits
  // are, plus at restore time — and it is the only one of the two that applies
  // evidence ranking, so a frame that may have been our own echo cannot demote
  // a fan whose band is already known (issue #31 review). Consulting the
  // report as a second source would reinstate exactly that demotion.
  // authority_speed_count is pure in the snapshot: a capability-less snapshot
  // (Forget, or a re-bind to a different fan) resets the count to the compiled
  // default rather than preserving the previous fan's band.
  supported_speed_count_ = authority_speed_count(authority);
  const auto confirmed = publication_gate_.next(authority);
  if (!confirmed) return;

  const auto feedback =
      authority_to_feedback(confirmed->state, supported_speed_count_);
  if (feedback.speed) speed = *feedback.speed;
  state = feedback.on;
  publish_state();
}

}  // namespace esphome::quietcool
