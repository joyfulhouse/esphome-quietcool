#pragma once

#include "quietcool/core/core_types.h"

#include <cstdint>

namespace quietcool {

class Millis32Widener final {
 public:
  MonotonicMs widen(std::uint32_t raw_ms);

 private:
  MonotonicMs epoch_ms_{0};
  std::uint32_t last_raw_ms_{0};
  bool sampled_{false};
};

}  // namespace quietcool
