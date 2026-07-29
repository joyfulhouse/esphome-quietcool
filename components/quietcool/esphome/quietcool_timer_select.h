#pragma once

#include "quietcool_component.h"
#include "timer_command.h"

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"

#include <optional>

namespace esphome::quietcool {

// The Fan Timer select: "None", "1 hour", "2 hours", "4 hours", "8 hours",
// "12 hours".
//
// EVERY option transmits an ENERGIZING command, "None" included. A timer set on
// a stopped fan STARTS it, at LOW; "None" means "no timer, run continuously"
// and so RESTARTS a fan whose timer had already expired. That is the
// maintainer's deliberate choice, matching the legacy YAML build, and it is why
// this entity is not a passive settings control.
//
// Like the fan entity, it publishes ONLY confirmed authority — never the user's
// own request. A refused command therefore leaves the shown option unchanged
// rather than briefly displaying a timer that was never programmed.
//
// This file deliberately holds no decision logic. It derives from select::Select
// and so is excluded from the adapter test binary; everything it decides lives
// in timer_command.{h,cpp}, which is linked and tested. That split is issue #15,
// where an untestable mapping welded into an entity could have been inverted
// with the whole suite green.
class QuietCoolTimerSelect final : public Component,
                                   public select::Select,
                                   public AuthorityPublisher {
 public:
  void set_controller(QuietCoolComponent* controller) {
    controller_ = controller;
    controller_->add_authority_publisher(this);
  }

  void dump_config() override;
  void publish_authority(
      const ::quietcool::AuthoritySnapshot& authority) override;

 protected:
  void control(const std::string& value) override;

 private:
  QuietCoolComponent* controller_{nullptr};
  // All snapshot-derived memory, updated exclusively by the linked, tested
  // timer_select_apply_snapshot — this file deliberately holds no update rule
  // of its own (rounds 1-3; see TimerSelectCache's comment). Held here rather
  // than read from the fan entity so the two entities stay independent.
  TimerSelectCache cache_;
};

}  // namespace esphome::quietcool
