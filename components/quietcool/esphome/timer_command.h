#pragma once

#include "quietcool/core/authority_store.h"
#include "quietcool/core/fan_state.h"

#include <cstdint>
#include <optional>
#include <string>

namespace esphome::quietcool {

// The durations a user may select for the fan to RUN. Deliberately not
// ::quietcool::Duration: that enum includes Off, which STOPS the fan (see
// Duration's own comment and the design doc §1). Reusing it here would let a
// future caller turn a timer request into a stop command — issue #30's failure
// shape by another route. Stopping the fan belongs to the fan entity alone.
enum class TimerSelection : std::uint8_t {
  Continuous, Hours1, Hours2, Hours4, Hours8, Hours12
};

// Maps a selection onto its wire duration nibble. Total; never yields Off.
::quietcool::Duration duration_for_selection(TimerSelection selection);

// Translates a timer selection into the FanState command driven onto the RF
// link. A timer command is speed|duration in one byte, so it is ENERGIZING: on
// a stopped fan it starts it, at LOW, matching the legacy YAML build. The speed
// nibble is mapped POSITIONALLY through speed_for_level(), against the COMMAND
// band — a 2-speed fan's top level is HIGH (0xB_), never MED (0xA_).
::quietcool::FanState timer_command_from_intent(TimerSelection requested, bool fan_on,
                                                int level,
                                                std::uint8_t command_speed_count);

// ---------------------------------------------------------------------------
// The select entity's vocabulary.
//
// These live here rather than in quietcool_timer_select.cpp because that file
// derives from select::Select and is excluded from the adapter test binary, so
// anything welded into it cannot be tested. That is issue #15: an untestable
// on/off mapping could have been inverted with the whole suite green.
// ---------------------------------------------------------------------------

// The option list, in the order Home Assistant renders it. "None" means no
// TIMER — the fan runs until stopped — and maps to Duration::Continuous. It
// does NOT mean Duration::Off, which stops the fan; the select cannot express
// that at all. Every option here transmits an ENERGIZING command.
inline constexpr const char* kTimerOptions[] = {
    "None", "1 hour", "2 hours", "4 hours", "8 hours", "12 hours"};

// Parses a Home Assistant option string. Returns nullopt for anything not in
// kTimerOptions, including case variants: an unrecognised option must transmit
// NOTHING rather than fall back to a guess, because every value this function
// can return puts an energizing command on the air.
std::optional<TimerSelection> selection_for_option(const std::string& option);

// The inverse. Total over the enum.
const char* option_for_selection(TimerSelection selection);

// The whole confirmed-state -> timer-command composition, in one linked and
// tested function: which band bounds the confirmed level (the ENTITY band) and
// which band forms the wire nibble (the COMMAND band). This pair-selection is
// the code that decides whether MED can reach the fan from the timer path, so
// it must not be composed inline in the untestable entity file (adversarial
// review, opus round 1: swapping the two bands there recreates issue #30 with
// every suite green).
//
// `confirmed` is the last CONFIRMED FanState, or nullopt when authority is not
// currently confirmed — invalidated by a timer expiry, a re-binding, or an
// in-flight command. nullopt and a confirmed OFF both take the documented
// stopped-fan rule: start at LOW. Passing a stale cache here instead of nullopt
// is the round-1 high finding: a fan whose timer expired, or a freshly-bound
// different fan, would be started at the PREVIOUS state's speed.
::quietcool::FanState timer_command_from_confirmed(
    const std::optional<::quietcool::FanState>& confirmed,
    std::optional<::quietcool::SpeedCapability> capability,
    TimerSelection requested);

// The option to publish for a confirmed authority snapshot, or nullopt to
// publish nothing at all.
//
// Nothing is published for an unknown timer authority: the select shows only
// what confirmed evidence supports, and asserting "None" from no evidence would
// claim the fan has no timer when we simply do not know. A Duration::Off inside
// a timer authority — which should not occur — degrades to "None" rather than
// inventing a stop option, keeping Off out of the timer vocabulary entirely.
std::optional<const char*> timer_option_for_authority(
    const ::quietcool::AuthoritySnapshot& authority);

}  // namespace esphome::quietcool
