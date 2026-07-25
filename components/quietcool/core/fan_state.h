#pragma once

#include "core_types.h"

namespace quietcool {

enum class Speed : std::uint8_t { Low = 1, Medium = 2, High = 3 };
enum class Duration : std::uint8_t {
  Off = 0, Hours1 = 1, Hours2 = 2, Hours4 = 4, Hours8 = 8, Hours12 = 12,
  Continuous = 15
};
enum class SpeedCapability : std::uint8_t { Unknown = 0, One = 1, Two = 2, Three = 3 };
enum class SpeedError : std::uint8_t { InvalidValue };
enum class DurationError : std::uint8_t { InvalidValue };
enum class StateError : std::uint8_t { InvalidDuration, RunningWithoutSpeed };

Result<Speed, SpeedError> speed_from_value(std::uint8_t value);
Result<Duration, DurationError> duration_from_value(std::uint8_t value);

class FanState final {
 public:
  static FanState command(Speed speed, Duration duration);
  static Result<FanState, StateError> observed(std::uint8_t raw);
  std::uint8_t raw_byte() const { return raw_; }
  std::uint8_t canonical_byte() const { return raw_ & 0x3FU; }
  std::uint8_t outbound_command_byte() const { return 0x80U | canonical_byte(); }
  std::optional<Speed> speed() const;
  Duration duration() const;
  std::optional<SpeedCapability> report_capability() const;
  bool is_on() const { return duration() != Duration::Off; }
  bool has_outbound_command_marker() const { return (raw_ & 0xC0U) == 0x80U; }
  bool semantically_equals(const FanState& other) const;
  bool operator==(const FanState& other) const { return canonical_byte() == other.canonical_byte(); }
 private:
  FanState(std::uint8_t raw, bool observed) : raw_(raw), observed_(observed) {}
  std::uint8_t raw_;
  bool observed_;
};

}  // namespace quietcool

