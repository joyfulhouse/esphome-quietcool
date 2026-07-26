#include "quietcool_button.h"

#include "esphome/core/log.h"

namespace esphome::quietcool {
namespace {

constexpr char TAG[] = "quietcool.button";

}  // namespace

void QuietCoolButton::dump_config() {
  LOG_BUTTON("", "QuietCool Controller Button", this);
}

void QuietCoolButton::press_action() {
  if (controller_ == nullptr) {
    ESP_LOGE(TAG, "Button press refused: controller is not configured");
    return;
  }
  // Forward the three provisioning actions to the controller. The kind -> action
  // routing itself lives in dispatch_button_press(), which is unit-tested; these
  // forwards are 1:1 by name.
  struct ControllerSink final : ButtonActionSink {
    explicit ControllerSink(QuietCoolComponent& c) : controller(c) {}
    QuietCoolComponent& controller;
    void manual_refresh() override { controller.request_manual_refresh(); }
    void learn() override {
      controller.request_learn(::quietcool::LearnMode::Manual);
    }
    void forget() override { controller.request_forget(); }
  };
  ControllerSink sink(*controller_);
  dispatch_button_press(kind_, sink);
}

}  // namespace esphome::quietcool
