#pragma once

#include "quietcool_component.h"

#include "esphome/core/automation.h"

#include <type_traits>
#include <utility>
#include <vector>

namespace esphome::quietcool {

template<typename... Ts> class QuietCoolOnPacketAction : public Action<Ts...> {
 public:
  explicit QuietCoolOnPacketAction(QuietCoolComponent* controller)
      : controller_(controller) {}

  void play(const std::vector<std::uint8_t>& packet, const float& rssi,
            const float& snr) override {
    (void) rssi;
    (void) snr;
    controller_->on_radio_packet(
        ::quietcool::ByteView(packet.data(), packet.size()));
  }

 private:
  QuietCoolComponent* controller_;
};

static_assert(
    std::is_base_of<
        Action<std::vector<std::uint8_t>, float, float>,
        QuietCoolOnPacketAction<std::vector<std::uint8_t>, float, float>>::value,
    "quietcool.on_packet must match the radio packet trigger signature");

}  // namespace esphome::quietcool
