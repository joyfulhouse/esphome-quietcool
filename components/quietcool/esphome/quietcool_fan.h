#pragma once

#include "esphome/components/quietcool/ports/authority_publication_gate.h"
#include "quietcool_component.h"

#include "esphome/components/fan/fan.h"
#include "esphome/core/component.h"

namespace esphome::quietcool {

class QuietCoolFan final : public Component,
                           public fan::Fan,
                           public AuthorityPublisher {
 public:
  void set_controller(QuietCoolComponent* controller) {
    controller_ = controller;
    controller_->set_authority_publisher(this);
  }

  void setup() override;
  void dump_config() override;
  fan::FanTraits get_traits() override;
  void publish_authority(
      const ::quietcool::AuthoritySnapshot& authority) override;

 protected:
  void control(const fan::FanCall& call) override;

 private:
  QuietCoolComponent* controller_{nullptr};
  ::quietcool::AuthorityPublicationGate publication_gate_;
  std::uint8_t supported_speed_count_{3};
};

}  // namespace esphome::quietcool
