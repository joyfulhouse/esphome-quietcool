#pragma once

// Pure formatting for the five restored text diagnostics (Task 4). No
// ESPHome dependency, so these are unit-testable directly — the same
// discipline timer_command.h used for the timer mapping.

#include "quietcool/core/authority_store.h"
#include "quietcool/core/sender_id.h"

#include <cstdint>
#include <optional>
#include <string>

namespace esphome::quietcool {

// "0xB1"-style two-digit uppercase hex. Shared by `Last TX Command` (the
// in-flight outbound command byte, already tracked for the capability echo
// guard — issue #31) and `Last Valid RX Frame` (the last accepted decoded
// frame's byte).
std::string format_hex_byte(std::uint8_t byte);

// "0xCB004739"-style eight-digit uppercase hex, or "unknown" while
// unprovisioned. Never fabricated: absent sender publishes "unknown", not a
// zeroed-out id that could be mistaken for a real one.
std::string format_sender_id(std::optional<::quietcool::SenderId> sender);

// "unknown" | "1" | "2" | "3". An absent (or explicitly Unknown) capability
// publishes "unknown" — inventing a band here is the exact defect class
// behind issue #30/#31, where a wrong speed band stopped a real fan.
std::string format_speed_capability(
    std::optional<::quietcool::SpeedCapability> capability);

// "off" | "low" | "medium" | "high", suffixed with the programmed duration
// (e.g. "high 1h") when the confirmed state carries one; no suffix while
// running Continuous (no timer to report). "unknown" whenever authority is
// not CONFIRMED — entities publish only confirmed authority, never an
// optimistic guess at what the fan is doing.
std::string format_last_confirmed_state(
    const ::quietcool::StateAuthority& state);

}  // namespace esphome::quietcool
