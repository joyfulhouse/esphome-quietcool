#pragma once

#include "quietcool/ports/authority_publication_gate.h"
#include "fan_feedback.h"
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
  // The entity's whole speed-band state, fed the sticky capability of every
  // published snapshot. Holding the band pair in one host-tested value keeps
  // the two counts — the one Home Assistant caches and the one an inbound level
  // is mapped against — from drifting apart, and keeps the listed band from
  // widening under a connection that cannot be re-listed (issue #31 review).
  FanSpeedBands bands_;
};

}  // namespace esphome::quietcool
