#include "diagnostic_format.h"

#include "quietcool/core/fan_state.h"

#include <cstdio>
#include <variant>

namespace esphome::quietcool {
namespace {

const char* speed_word(::quietcool::Speed speed) {
  switch (speed) {
    case ::quietcool::Speed::Low: return "low";
    case ::quietcool::Speed::Medium: return "medium";
    case ::quietcool::Speed::High: return "high";
  }
  return "unknown";
}

// Empty for Off (unreachable here — the caller handles !is_on() first) and
// for Continuous (a running fan with no timer counting down — §1 of the
// design doc: Off and Continuous are opposites, not redundant).
const char* duration_suffix(::quietcool::Duration duration) {
  switch (duration) {
    case ::quietcool::Duration::Hours1: return " 1h";
    case ::quietcool::Duration::Hours2: return " 2h";
    case ::quietcool::Duration::Hours4: return " 4h";
    case ::quietcool::Duration::Hours8: return " 8h";
    case ::quietcool::Duration::Hours12: return " 12h";
    case ::quietcool::Duration::Off:
    case ::quietcool::Duration::Continuous:
      return "";
  }
  return "";
}

}  // namespace

std::string format_hex_byte(std::uint8_t byte) {
  char buffer[5];
  std::snprintf(buffer, sizeof(buffer), "0x%02X", static_cast<unsigned int>(byte));
  return std::string(buffer);
}

std::string format_sender_id(std::optional<::quietcool::SenderId> sender) {
  if (!sender) return "unknown";
  char buffer[11];
  std::snprintf(buffer, sizeof(buffer), "0x%08X",
               static_cast<unsigned int>(sender->as_be_u32()));
  return std::string(buffer);
}

std::string format_speed_capability(
    std::optional<::quietcool::SpeedCapability> capability) {
  if (!capability) return "unknown";
  switch (*capability) {
    case ::quietcool::SpeedCapability::One: return "1";
    case ::quietcool::SpeedCapability::Two: return "2";
    case ::quietcool::SpeedCapability::Three: return "3";
    case ::quietcool::SpeedCapability::Unknown: return "unknown";
  }
  return "unknown";
}

std::string format_last_confirmed_state(
    const ::quietcool::StateAuthority& state) {
  const auto* confirmed =
      std::get_if<::quietcool::ConfirmedStateAuthority>(&state);
  if (confirmed == nullptr) return "unknown";
  const auto& fan_state = confirmed->state;
  if (!fan_state.is_on()) return "off";
  const auto speed = fan_state.speed();
  if (!speed) return "unknown";
  return std::string(speed_word(*speed)) + duration_suffix(fan_state.duration());
}

}  // namespace esphome::quietcool
