#include "millis32_widener.h"

namespace quietcool {

MonotonicMs Millis32Widener::widen(std::uint32_t raw_ms) {
  if (sampled_ && raw_ms < last_raw_ms_)
    epoch_ms_ += MonotonicMs{1} << 32U;
  sampled_ = true;
  last_raw_ms_ = raw_ms;
  return epoch_ms_ + raw_ms;
}

}  // namespace quietcool
