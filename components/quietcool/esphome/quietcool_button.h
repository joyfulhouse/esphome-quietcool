#pragma once

#include "button_dispatch.h"
#include "quietcool_component.h"

#include "esphome/components/button/button.h"
#include "esphome/core/component.h"

namespace esphome::quietcool {

// QuietCoolButtonKind lives in button_dispatch.h so the kind -> action routing
// stays testable without this ESPHome-derived class.

class QuietCoolButton final : public Component, public button::Button {
 public:
  void set_controller(QuietCoolComponent* controller) {
    controller_ = controller;
  }
  void set_kind(QuietCoolButtonKind kind) { kind_ = kind; }
  void dump_config() override;

 protected:
  void press_action() override;

 private:
  QuietCoolComponent* controller_{nullptr};
  QuietCoolButtonKind kind_{QuietCoolButtonKind::Refresh};
};

}  // namespace esphome::quietcool
