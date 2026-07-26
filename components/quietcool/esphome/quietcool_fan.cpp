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
  const auto confirmed = publication_gate_.next(authority);
  if (!confirmed) return;

  const auto feedback = authority_to_feedback(confirmed->state);
  if (feedback.supported_speed_count)
    supported_speed_count_ = *feedback.supported_speed_count;
  if (feedback.speed) speed = *feedback.speed;
  state = feedback.on;
  publish_state();
}

}  // namespace esphome::quietcool
