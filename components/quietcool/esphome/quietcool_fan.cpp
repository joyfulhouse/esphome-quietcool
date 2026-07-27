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
  // The ENTITY band. Home Assistant reads this once per API connection, at
  // ListEntities, and caches it until it reconnects — so it must never widen
  // within a session. FanSpeedBands starts at the widest band and latches it
  // monotonically non-increasing (issue #31 review).
  traits.set_supported_speed_count(bands_.entity());
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
  // The level is logged in the entity's vocabulary — the band Home Assistant
  // was listed — but mapped to the wire against the COMMAND band, which is
  // never wider and, while capability is unknown, cannot form MED, the speed a
  // 2-speed fan lacks (issue #31 review). Against a narrower band
  // speed_for_level saturates, so the top of Home Assistant's band is always
  // HIGH.
  ESP_LOGD(TAG, "FanCall queued: state=%s speed=%d; awaiting confirmation",
           ONOFF(requested_state),
           clamp_fan_speed(requested_speed, bands_.entity()));
  controller_->request_state(fan_command_from_intent(
      requested_state, requested_speed, bands_.command()));
}

void QuietCoolFan::publish_authority(
    const ::quietcool::AuthoritySnapshot& authority) {
  // Seed the capability BEFORE the publication gate (issue #31): the
  // restore-time publication carries no confirmed state — the gate swallows
  // it — but its restored speed_capability must reach get_traits() before
  // Home Assistant can send a command, or a level-2 press on a 2-speed fan is
  // mapped against a wider band and transmits MED, which stops the fan (issue
  // #30's failure, transiently).
  //
  // The snapshot's sticky capability is the SOLE input to both bands. promote()
  // folds every confirmed report's capability into the snapshot and it is the
  // only place evidence ranking is applied, so a frame that may have been our
  // own echo (marker bits 10 alias capability Two) cannot demote a fan whose
  // band is already known (issue #31 review). authority_to_feedback is handed
  // the ENTITY band and never re-derives one from the report, which is what
  // keeps the published LEVEL inside the band Home Assistant was listed.
  //
  // Fed wholesale, never merged with a cached count: a capability-less snapshot
  // (Forget, or a re-bind to a different fan) returns the command band to the
  // unlearned MED-free one rather than preserving the previous fan's.
  bands_.observe(authority.speed_capability);
  const auto confirmed = publication_gate_.next(authority);
  if (!confirmed) return;

  const auto feedback =
      authority_to_feedback(confirmed->state, bands_.entity());
  if (feedback.speed) speed = *feedback.speed;
  state = feedback.on;
  publish_state();
}

}  // namespace esphome::quietcool
