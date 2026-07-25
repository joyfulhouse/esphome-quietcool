#pragma once

#include "esphome/components/quietcool/core/core_types.h"

namespace quietcool {

enum class RadioSendResult : std::uint8_t { Sent, Rejected, Fault };
enum class RadioRecoveryResult : std::uint8_t { Recovered, Fault };

class Radio {
 public:
  virtual ~Radio();
  virtual RadioSendResult send_packet(const FrameBytes& payload) = 0;
  virtual RadioRecoveryResult recover() = 0;
};

}  // namespace quietcool
