#include "sx127x_radio_adapter.h"

#ifdef QUIETCOOL_USE_SX127X
#include <vector>

namespace esphome::quietcool {

::quietcool::RadioSendResult Sx127xRadioAdapter::send_packet(
    const ::quietcool::FrameBytes& payload) {
  const std::vector<std::uint8_t> packet(payload.bytes.begin(),
                                         payload.bytes.end());
  const auto result = radio_.transmit_packet(packet);
  if (result == sx127x::SX127xError::NONE)
    return ::quietcool::RadioSendResult::Sent;
  if (result == sx127x::SX127xError::INVALID_PARAMS)
    return ::quietcool::RadioSendResult::Rejected;
  return ::quietcool::RadioSendResult::Fault;
}

::quietcool::RadioRecoveryResult Sx127xRadioAdapter::recover() {
  radio_.configure();
  return radio_.is_failed() ? ::quietcool::RadioRecoveryResult::Fault
                            : ::quietcool::RadioRecoveryResult::Recovered;
}

}  // namespace esphome::quietcool
#endif
