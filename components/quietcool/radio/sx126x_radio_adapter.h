#pragma once

#include "quietcool/ports/radio.h"
#include "esphome/core/defines.h"

#ifdef QUIETCOOL_USE_SX126X
#include "esphome/components/sx126x/sx126x.h"

namespace esphome::quietcool {

class Sx126xRadioAdapter final : public ::quietcool::Radio {
 public:
  explicit Sx126xRadioAdapter(sx126x::SX126x* radio) : radio_(*radio) {}
  ::quietcool::RadioSendResult send_packet(
      const ::quietcool::FrameBytes& payload) override;
  ::quietcool::RadioRecoveryResult recover() override;

 private:
  sx126x::SX126x& radio_;
};

}  // namespace esphome::quietcool
#endif
