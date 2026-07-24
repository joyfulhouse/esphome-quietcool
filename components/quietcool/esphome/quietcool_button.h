#pragma once

#include "quietcool_component.h"

#include "esphome/components/button/button.h"
#include "esphome/core/component.h"

#include <cstdint>

namespace esphome::quietcool {

enum class QuietCoolButtonKind : std::uint8_t { Refresh, Learn, Forget };

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
