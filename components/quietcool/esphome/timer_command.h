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

// The select's cache-update rule: what the cached confirmed state should be
// AFTER seeing this snapshot, given what it was before. Extracted here — in
// the linked, tested unit — because both wrong versions of this rule were
// found adversarially in an entity file no test can reach:
//   - LATCHING through invalidations (round 1) aimed energizing commands with
//     a state the fan no longer had (expired timer, re-bound fan);
//   - CLEARING on every invalidation (round 2) turned "set the fan HIGH, then
//     set a timer" into LOW+duration, because begin_local_command invalidates
//     authority on EVERY accepted command.
// The discrimination: a confirmed snapshot replaces the cache; an unknown
// snapshot clears it only when its reason changes WHAT the state describes
// (Unprovisioned, SenderChanged, LearningStarted, EstimatedTimerDeadline);
// every freshness reason — and revalidation — keeps it, because the fan is
// still physically doing what was last confirmed.
std::optional<::quietcool::FanState> confirmed_state_after(
    const ::quietcool::AuthoritySnapshot& authority,
    const std::optional<::quietcool::FanState>& previous);

// Everything the select entity remembers between snapshots, held in one value
// so the whole per-snapshot update is a single linked, tested call. Round 3
// proved by mutation that logic left in quietcool_timer_select.cpp is
// unreachable by any test in this repo (the file is excluded from
// ADAPTER_SOURCES): deleting its cache update left every suite green.
struct TimerSelectCache final {
  std::optional<::quietcool::FanState> confirmed;
  std::optional<::quietcool::SpeedCapability> capability;
  // The earliest moment the current timer is PROVEN to have run out —
  // an anchored deadline, or a programmed duration's observation time plus
  // its length. Persisted here because a freshness invalidation of the TIMER
  // authority (a Refresh, an OEM poll) discards the variant carrying the
  // deadline while the speed cache rightly survives (round 11, codex): a
  // press after the real expiry must still take the stopped-fan LOW rule.
  // Cleared by a confirmed no-active-timer and by identity invalidations —
  // fan A's deadline must not presume fan B stopped.
  std::optional<::quietcool::MonotonicMs> expiry_bound;
};

// Applies one authority snapshot to the cache and returns the option to
// publish — already deduplicated against the currently shown option, because
// snapshots arrive every effect-drain round and an undeduped republish is one
// native-API message per loop tick (round 3, opus, measured). `shown_option`
// is the entity's current option, or ""/nullptr when it has none.
std::optional<const char*> timer_select_apply_snapshot(
    TimerSelectCache& cache, const ::quietcool::AuthoritySnapshot& authority,
    const char* shown_option);

// The command for a user press, composed against the CURRENT snapshot. Beyond
// timer_command_from_confirmed it applies the presumed-stopped rule: a timer
// that has provably run out means the fan is presumed stopped even if the
// snapshot still shows a confirmed running state, and the press composes from
// the stopped-fan LOW rule rather than restarting the stopped fan at its old
// speed. Two ways a timer proves out (rounds 6-7):
//   - a locally anchored deadline that has passed — poll() processes it, but
//     a press can land in the tick where it is due yet unprocessed;
//   - a ProgrammedDurationAuthority whose observation time plus duration has
//     passed — the core now fires EstimatedTimerDeadline for this variant at
//     the same observation-time upper bound (issue #35), but a press can
//     still race the poll that would process it, so the rule stays here too.
// The cache is taken by reference and cache.confirmed is CLEARED when the
// rule fires: the press's own request_state invalidates the timer authority,
// destroying the evidence this presumption is derived from, so without
// persisting it only the first press composed correctly (round 7, opus).
::quietcool::FanState timer_command_for_press(
    TimerSelectCache& cache,
    const ::quietcool::AuthoritySnapshot& authority,
    ::quietcool::MonotonicMs now_ms, TimerSelection requested);

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
