#include "quietcool_fan.h"

#include "esphome/core/log.h"

namespace esphome::quietcool {
namespace {

constexpr char TAG[] = "quietcool.fan";

int clamp_speed(int speed, std::uint8_t supported_speed_count) {
  if (speed < 1) return 1;
  if (speed > supported_speed_count) return supported_speed_count;
  return speed;
}

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
  const auto requested_state =
      call.get_state().has_value() ? *call.get_state() : state;
  const auto requested_speed = clamp_speed(
      call.get_speed().has_value() ? *call.get_speed() : speed,
      supported_speed_count_);
  const auto typed_speed =
      static_cast<::quietcool::Speed>(requested_speed);
  const auto duration = requested_state ? ::quietcool::Duration::Continuous
                                        : ::quietcool::Duration::Off;
  ESP_LOGD(TAG, "FanCall queued: state=%s speed=%d; awaiting confirmation",
           ONOFF(requested_state), requested_speed);
  controller_->request_state(
      ::quietcool::FanState::command(typed_speed, duration));
}

void QuietCoolFan::publish_authority(
    const ::quietcool::AuthoritySnapshot& authority) {
  const auto confirmed = publication_gate_.next(authority);
  if (!confirmed) return;

  if (const auto capability = confirmed->state.report_capability()) {
    const auto count = static_cast<std::uint8_t>(*capability);
    if (count >= 1 && count <= 3) supported_speed_count_ = count;
  }
  if (const auto confirmed_speed = confirmed->state.speed())
    speed = static_cast<int>(*confirmed_speed);
  state = confirmed->state.is_on();
  publish_state();
}

}  // namespace esphome::quietcool
