#include "fan_state.h"

namespace quietcool {

Result<Speed, SpeedError> speed_from_value(std::uint8_t value) {
  if (value >= 1 && value <= 3)
    return Result<Speed, SpeedError>::ok(static_cast<Speed>(value));
  return Result<Speed, SpeedError>::err(SpeedError::InvalidValue);
}

Result<Duration, DurationError> duration_from_value(std::uint8_t value) {
  switch (value) {
    case 0: case 1: case 2: case 4: case 8: case 12: case 15:
      return Result<Duration, DurationError>::ok(static_cast<Duration>(value));
    default: return Result<Duration, DurationError>::err(DurationError::InvalidValue);
  }
}

FanState FanState::command(Speed speed, Duration duration) {
  return FanState(static_cast<std::uint8_t>(0x80U |
      (static_cast<std::uint8_t>(speed) << 4U) |
      static_cast<std::uint8_t>(duration)), false);
}

Result<FanState, StateError> FanState::observed(std::uint8_t raw) {
  const auto duration_result = duration_from_value(raw & 0x0FU);
  if (!duration_result) return Result<FanState, StateError>::err(StateError::InvalidDuration);
  if (((raw >> 4U) & 0x03U) == 0 && duration_result.value() != Duration::Off)
    return Result<FanState, StateError>::err(StateError::RunningWithoutSpeed);
  return Result<FanState, StateError>::ok(FanState(raw, true));
}

bool FanState::is_valid_command() const {
  return has_outbound_command_marker() && speed().has_value() &&
         duration_from_value(raw_ & 0x0FU).has_value();
}

std::optional<Speed> FanState::speed() const {
  const auto result = speed_from_value((raw_ >> 4U) & 0x03U);
  return result ? std::optional<Speed>(result.value()) : std::nullopt;
}

Duration FanState::duration() const {
  return static_cast<Duration>(raw_ & 0x0FU);
}

std::optional<SpeedCapability> FanState::report_capability() const {
  if (!observed_) return std::nullopt;
  return static_cast<SpeedCapability>((raw_ >> 6U) & 0x03U);
}

bool FanState::semantically_equals(const FanState& other) const {
  return (!is_on() && !other.is_on()) || canonical_byte() == other.canonical_byte();
}

}  // namespace quietcool

