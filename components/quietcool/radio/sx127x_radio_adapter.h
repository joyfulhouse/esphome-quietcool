#pragma once

#include "esphome/components/quietcool/ports/radio.h"
#include "esphome/core/defines.h"

#ifdef QUIETCOOL_USE_SX127X
#include "esphome/components/sx127x/sx127x.h"

namespace esphome::quietcool {

class Sx127xRadioAdapter final : public ::quietcool::Radio {
 public:
  explicit Sx127xRadioAdapter(sx127x::SX127x* radio) : radio_(*radio) {}
  ::quietcool::RadioSendResult send_packet(
      const ::quietcool::FrameBytes& payload) override;
  ::quietcool::RadioRecoveryResult recover() override;

 private:
  sx127x::SX127x& radio_;
};

}  // namespace esphome::quietcool
#endif
