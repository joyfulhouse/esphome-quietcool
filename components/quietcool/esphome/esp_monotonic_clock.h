#pragma once

#include "quietcool/ports/clock.h"
#include "quietcool/ports/millis32_widener.h"

namespace esphome::quietcool {

class EspMonotonicClock final : public ::quietcool::Clock {
 public:
  ::quietcool::MonotonicMs now_ms() const override;

 private:
  mutable ::quietcool::Millis32Widener widener_;
};

}  // namespace esphome::quietcool
