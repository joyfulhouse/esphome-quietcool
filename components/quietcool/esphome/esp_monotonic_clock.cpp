#include "esp_monotonic_clock.h"

#include "esphome/core/hal.h"

namespace esphome::quietcool {

::quietcool::MonotonicMs EspMonotonicClock::now_ms() const {
  return widener_.widen(static_cast<std::uint32_t>(millis()));
}

}  // namespace esphome::quietcool
