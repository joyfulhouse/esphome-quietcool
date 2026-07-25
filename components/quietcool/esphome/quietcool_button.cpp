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
  switch (kind_) {
    case QuietCoolButtonKind::Refresh:
      controller_->request_manual_refresh();
      break;
    case QuietCoolButtonKind::Learn:
      controller_->request_learn(::quietcool::LearnMode::Manual);
      break;
    case QuietCoolButtonKind::Forget:
      controller_->request_forget();
      break;
  }
}

}  // namespace esphome::quietcool
