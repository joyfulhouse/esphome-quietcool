#pragma once

// Host stub. Real ESPHome returns a free-running 32-bit millisecond counter
// that wraps every ~49.7 days; the widener under test exists to survive that
// wrap, so tests must be able to place the counter anywhere in its range.

#include <cstdint>

namespace esphome {

namespace host_test {

inline std::uint32_t& millis_value() {
  static std::uint32_t value = 0;
  return value;
}

inline void set_millis(std::uint32_t value) { millis_value() = value; }
inline void advance_millis(std::uint32_t delta) { millis_value() += delta; }

}  // namespace host_test

inline std::uint32_t millis() { return host_test::millis_value(); }

}  // namespace esphome
