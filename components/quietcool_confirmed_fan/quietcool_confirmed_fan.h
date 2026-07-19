#pragma once

#include "esphome/components/fan/fan.h"
#include "esphome/components/script/script.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome::quietcool_confirmed_fan {

static const char *const TAG = "quietcool.fan";

// A fan command and a fan observation are different events. The stock
// TemplateFan writes the requested state and publishes it from control()
// before the radio transaction can be confirmed. For QuietCool that made an
// optimistic OFF indistinguishable from a physical OFF to Home Assistant.
//
// This platform deliberately leaves state/speed untouched in control(). A
// call only starts the appropriate command script. Only the YAML coordinator
// may publish query-correlated authoritative state. Passive RF and estimated
// timer expiry deliberately leave the fan value unchanged and invalidate the
// separate Known/Confirmed-Off diagnostics instead.
class QuietCoolFan final : public Component, public fan::Fan {
 public:
  void set_off_script(script::Script<> *command_script) { this->off_script_ = command_script; }
  void set_low_script(script::Script<> *command_script) { this->low_script_ = command_script; }
  void set_medium_script(script::Script<> *command_script) { this->medium_script_ = command_script; }
  void set_high_script(script::Script<> *command_script) { this->high_script_ = command_script; }

  void setup() override {
    // Raw initialization only: never restore or publish at boot. It preserves
    // the safe bare-turn-on default without generating an entity callback or
    // RF command. The first authoritative query-correlated consensus owns
    // public state.
    if (this->speed == 0)
      this->speed = 1;
  }

  void dump_config() override { LOG_FAN("", "QuietCool Confirmed Fan", this); }

  fan::FanTraits get_traits() override {
    fan::FanTraits traits{};
    traits.set_speed(true);
    traits.set_supported_speed_count(3);
    return traits;
  }

 protected:
  void control(const fan::FanCall &call) override {
    const auto state_request = call.get_state();
    const auto speed_request = call.get_speed();
    const bool requested_state = state_request.has_value() ? *state_request : this->state;
    int requested_speed = speed_request.has_value() ? *speed_request : this->speed;
    if (requested_speed < 1)
      requested_speed = 1;
    if (requested_speed > 3)
      requested_speed = 3;

    ESP_LOGD(TAG, "Command requested: state=%s speed=%d; awaiting RF confirmation",
             ONOFF(requested_state), requested_speed);

    if (!requested_state) {
      if (this->off_script_ != nullptr)
        this->off_script_->execute();
      return;
    }
    if (requested_speed <= 1) {
      if (this->low_script_ != nullptr)
        this->low_script_->execute();
    } else if (requested_speed == 2) {
      if (this->medium_script_ != nullptr)
        this->medium_script_->execute();
    } else if (this->high_script_ != nullptr) {
      this->high_script_->execute();
    }
  }

  script::Script<> *off_script_{nullptr};
  script::Script<> *low_script_{nullptr};
  script::Script<> *medium_script_{nullptr};
  script::Script<> *high_script_{nullptr};
};

}  // namespace esphome::quietcool_confirmed_fan
