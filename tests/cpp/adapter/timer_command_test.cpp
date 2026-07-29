// Timer actuation mapping: TimerSelection -> FanState command byte.
//
// A timer command is speed|duration in ONE byte, so it is an energizing
// command and it carries a speed nibble like any other. Two failure modes are
// guarded here: expressing Duration::Off (which would STOP a fan the user
// asked to run for N hours), and mapping speed by identity (which on a
// 2-speed fan transmits MED, a speed it does not have — issue #30).

#include "quietcool/esphome/timer_command.h"

#include "quietcool/core/fan_state.h"

#include "support/test.h"

#include <cstdint>
#include <string>

namespace esphome::quietcool {
namespace {

using ::quietcool::Duration;
using ::quietcool::FanState;
using ::quietcool::Speed;

constexpr std::uint8_t kTwoSpeeds = 2;
constexpr std::uint8_t kThreeSpeeds = 3;

QC_TEST("timer_command", "timer selection maps to its duration") {
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Continuous), Duration::Continuous);
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Hours1), Duration::Hours1);
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Hours2), Duration::Hours2);
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Hours4), Duration::Hours4);
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Hours8), Duration::Hours8);
  QC_CHECK_EQ(duration_for_selection(TimerSelection::Hours12), Duration::Hours12);
}

QC_TEST("timer_command", "no selection can produce a stop command") {
  // Every selection must leave the fan running. Duration::Off is the stop
  // command; it must be unreachable from the timer path.
  const TimerSelection all[] = {
      TimerSelection::Continuous, TimerSelection::Hours1, TimerSelection::Hours2,
      TimerSelection::Hours4,     TimerSelection::Hours8, TimerSelection::Hours12};
  for (const auto selection : all) {
    const auto command = timer_command_from_intent(selection, true, 1, kThreeSpeeds);
    QC_CHECK(command.is_on());
    QC_CHECK(command.duration() != Duration::Off);
  }
}

QC_TEST("timer_command", "running fan keeps its speed and takes the duration") {
  // High + 1 hour == 0xB1, per the legacy byte table.
  const auto command = timer_command_from_intent(TimerSelection::Hours1, true, 3, kThreeSpeeds);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xB1);
  QC_CHECK_EQ(command.speed().value(), Speed::High);
  QC_CHECK_EQ(command.duration(), Duration::Hours1);
}

QC_TEST("timer_command", "stopped fan defaults to low and starts") {
  // Legacy behavior, deliberately retained: a timer set on a stopped fan
  // STARTS it at LOW. Low + 2 hours == 0x92.
  const auto command = timer_command_from_intent(TimerSelection::Hours2, false, 3, kThreeSpeeds);
  QC_CHECK_EQ(command.outbound_command_byte(), 0x92);
  QC_CHECK_EQ(command.speed().value(), Speed::Low);
  QC_CHECK(command.is_on());
}

QC_TEST("timer_command", "two speed top level takes high never medium") {
  // The band is positional: level 2 of 2 is HIGH (0xB_), never MED (0xA_).
  // An identity cast here re-creates issue #30 through the timer path.
  const auto command = timer_command_from_intent(TimerSelection::Hours4, true, 2, kTwoSpeeds);
  QC_CHECK_EQ(command.speed().value(), Speed::High);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xB4);
}

QC_TEST("timer_command", "continuous selection matches the fan entity's on command") {
  // Selecting "None" (Continuous) must produce exactly what turning the fan on
  // produces, or the two controls would disagree about what "running" means.
  const auto command = timer_command_from_intent(TimerSelection::Continuous, true, 3, kThreeSpeeds);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xBF);
}

// ---------------------------------------------------------------------------
// Option strings and authority feedback.
//
// These live here, not in quietcool_timer_select.cpp, because that file derives
// from select::Select and is excluded from this test binary — the same reason
// fan_command_from_intent was extracted in issue #15. Logic welded into an
// unlinkable entity file can be inverted with the whole suite still green.
// ---------------------------------------------------------------------------

QC_TEST("timer_command", "option strings round trip through selection") {
  for (const auto* option : kTimerOptions) {
    const auto selection = selection_for_option(option);
    QC_CHECK(selection.has_value());
    QC_CHECK(std::string(option_for_selection(selection.value())) == std::string(option));
  }
}

QC_TEST("timer_command", "none is the continuous option") {
  // "None" means no TIMER, which on the wire is Continuous — the fan runs until
  // stopped. It does NOT mean Duration::Off, which would stop the fan.
  QC_CHECK_EQ(selection_for_option("None").value(), TimerSelection::Continuous);
  QC_CHECK(std::string(option_for_selection(TimerSelection::Continuous)) == "None");
}

QC_TEST("timer_command", "an unknown option is refused not guessed") {
  // A refusal must transmit nothing. Guessing a duration here would put an
  // energizing command on the air that the user never chose.
  QC_CHECK(!selection_for_option("3 hours").has_value());
  QC_CHECK(!selection_for_option("").has_value());
  QC_CHECK(!selection_for_option("none").has_value());
}

QC_TEST("timer_command", "unknown timer authority publishes nothing") {
  // Nothing confirmed means nothing to publish. Publishing "None" here would
  // assert a fact about the fan that no evidence supports.
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::UnknownTimerAuthority{};
  QC_CHECK(!timer_option_for_authority(snapshot).has_value());
}

QC_TEST("timer_command", "a programmed timer publishes its duration") {
  ::quietcool::AuthoritySnapshot snapshot{};
  ::quietcool::ProgrammedDurationAuthority programmed{};
  programmed.duration = ::quietcool::Duration::Hours4;
  snapshot.timer = programmed;
  QC_CHECK(std::string(timer_option_for_authority(snapshot).value()) == "4 hours");
}

QC_TEST("timer_command", "a locally anchored timer publishes its duration") {
  // Aggregate-initialised in full: TransactionId/AttemptNumber are opaque ids
  // with no default constructor, so `{}` does not compile here.
  ::quietcool::AuthoritySnapshot snapshot{};
  const ::quietcool::LocallyAnchoredTimerAuthority anchored{
      ::quietcool::Duration::Hours12,
      ::quietcool::TransactionId(1),
      ::quietcool::AttemptNumber(1),
      0,
      0,
      ::quietcool::EvidenceSource::PostCommandConsensus};
  snapshot.timer = anchored;
  QC_CHECK(std::string(timer_option_for_authority(snapshot).value()) == "12 hours");
}

QC_TEST("timer_command", "no active timer publishes none") {
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::NoActiveTimerAuthority{};
  QC_CHECK(std::string(timer_option_for_authority(snapshot).value()) == "None");
}

QC_TEST("timer_command", "every duration option maps back to its own string") {
  // option_for_duration must be DERIVED from kTimerOptions, not a second list
  // of string literals: a coordinated rename of an option in select.py and
  // kTimerOptions would otherwise leave the feedback path publishing the old
  // string, which ESPHome's Select rejects as an invalid option — the entity
  // goes permanently stateless in HA while still transmitting (adversarial
  // review, opus round 1). Every timed duration is asserted, not a sample.
  const struct { ::quietcool::Duration duration; const char* option; } cases[] = {
      {::quietcool::Duration::Hours1, "1 hour"},
      {::quietcool::Duration::Hours2, "2 hours"},
      {::quietcool::Duration::Hours4, "4 hours"},
      {::quietcool::Duration::Hours8, "8 hours"},
      {::quietcool::Duration::Hours12, "12 hours"},
      {::quietcool::Duration::Continuous, "None"},
  };
  for (const auto& c : cases) {
    ::quietcool::AuthoritySnapshot snapshot{};
    ::quietcool::ProgrammedDurationAuthority programmed{};
    programmed.duration = c.duration;
    snapshot.timer = programmed;
    const auto option = timer_option_for_authority(snapshot);
    QC_CHECK(option.has_value());
    QC_CHECK(std::string(option.value()) == c.option);
    // And the published string must be one the entity's option list contains,
    // or Select::publish_state refuses it.
    QC_CHECK(selection_for_option(option.value()).has_value());
  }
}

// ---------------------------------------------------------------------------
// The full confirmed-state -> command composition. This is the code that
// decides whether MED can reach the fan from the timer path, so it lives here
// as ONE linked function rather than being composed inline in the untestable
// entity file (adversarial review, opus round 1: with the two band calls
// swapped, a timer on a running 2-speed fan transmits MED and stops it, and
// every suite stays green).
// ---------------------------------------------------------------------------

QC_TEST("timer_command", "confirmed high with unknown capability stays high") {
  // Band round trip under unknown capability: entity band 3 in, command band 2
  // out. A confirmed HIGH must come back out as HIGH, never MED.
  const auto confirmed = FanState::command(Speed::High, Duration::Continuous);
  const auto command = timer_command_from_confirmed(
      confirmed, std::nullopt, TimerSelection::Hours2);
  QC_CHECK_EQ(command.speed().value(), Speed::High);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xB2);
}

QC_TEST("timer_command", "confirmed medium with unknown capability cannot resend medium") {
  // The safety property the band split exists for: while capability is
  // unknown the command band structurally cannot form MED. A stale confirmed
  // MED clamps to HIGH — one press in the safe direction, never a stop.
  const auto confirmed = FanState::command(Speed::Medium, Duration::Continuous);
  const auto command = timer_command_from_confirmed(
      confirmed, std::nullopt, TimerSelection::Hours1);
  QC_CHECK(command.speed().value() != Speed::Medium);
  QC_CHECK_EQ(command.speed().value(), Speed::High);
}

QC_TEST("timer_command", "confirmed medium with confirmed three speeds keeps medium") {
  const auto confirmed = FanState::command(Speed::Medium, Duration::Continuous);
  const auto command = timer_command_from_confirmed(
      confirmed, ::quietcool::SpeedCapability::Three, TimerSelection::Hours4);
  QC_CHECK_EQ(command.speed().value(), Speed::Medium);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xA4);
}

QC_TEST("timer_command", "no confirmed state is a stopped fan and starts at low") {
  // The stale-cache fix (codex high + opus medium, round 1): when authority is
  // invalidated — timer expiry, re-binding, in-flight command — the caller
  // passes nullopt, and the command falls back to the documented stopped-fan
  // rule: start at LOW. Never the previous fan's speed.
  const auto command = timer_command_from_confirmed(
      std::nullopt, std::nullopt, TimerSelection::Hours2);
  QC_CHECK_EQ(command.outbound_command_byte(), 0x92);
  QC_CHECK_EQ(command.speed().value(), Speed::Low);
}

QC_TEST("timer_command", "a confirmed stopped fan starts at low not its remembered speed") {
  // confirmed_ holding an OFF report must behave exactly like no report: LOW.
  const auto confirmed = FanState::command(Speed::High, Duration::Off);
  const auto command = timer_command_from_confirmed(
      confirmed, ::quietcool::SpeedCapability::Three, TimerSelection::Hours8);
  QC_CHECK_EQ(command.speed().value(), Speed::Low);
  QC_CHECK_EQ(command.outbound_command_byte(), 0x98);
}

// ---------------------------------------------------------------------------
// confirmed_state_after: the select's cache-update rule, extracted and tested
// because BOTH round-1 shapes of getting it wrong were found adversarially:
// latching through invalidations aimed energizing commands with a state the
// fan no longer had (round 1), and clearing on EVERY invalidation turned the
// primary journey — set the fan HIGH, then set a timer — into LOW+duration,
// because begin_local_command invalidates on every accepted command (round 2,
// opus high). Only reasons that change WHAT the state describes clear the
// cache; freshness reasons keep it.
// ---------------------------------------------------------------------------

namespace {
::quietcool::AuthoritySnapshot snapshot_with_unknown(
    ::quietcool::AuthorityLossReason reason) {
  ::quietcool::AuthoritySnapshot snapshot{};
  ::quietcool::UnknownStateAuthority unknown{};
  unknown.reason = reason;
  snapshot.state = unknown;
  return snapshot;
}
}  // namespace

QC_TEST("timer_command", "a confirmed snapshot replaces the cache") {
  ::quietcool::AuthoritySnapshot snapshot{};
  const auto high = FanState::command(Speed::High, Duration::Continuous);
  // Aggregate-initialised in full: FanState has no default constructor.
  snapshot.state = ::quietcool::ConfirmedStateAuthority{
      high,
      ::quietcool::EvidenceSource::ManualQueryConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      0,
      2,
      std::nullopt,
      std::nullopt,
      1};
  const auto next = confirmed_state_after(snapshot, std::nullopt);
  QC_CHECK(next.has_value());
  QC_CHECK_EQ(next->canonical_byte(), high.canonical_byte());
}

QC_TEST("timer_command", "a pending local command keeps the cache") {
  // The primary journey: user sets the fan HIGH, then sets a timer while the
  // command is still confirming. begin_local_command invalidates authority
  // with LocalCommandPending on EVERY accepted command, so clearing here made
  // the follow-up timer transmit LOW and silently discard the chosen speed.
  const auto high = FanState::command(Speed::High, Duration::Continuous);
  const auto next = confirmed_state_after(
      snapshot_with_unknown(::quietcool::AuthorityLossReason::LocalCommandPending),
      high);
  QC_CHECK(next.has_value());
  QC_CHECK_EQ(next->canonical_byte(), high.canonical_byte());
}

QC_TEST("timer_command", "freshness invalidations keep the cache") {
  // OEM remote polls, consensus timeouts, our own pending work: we are
  // momentarily unsure, but nothing claims the fan moved.
  const auto high = FanState::command(Speed::High, Duration::Continuous);
  const ::quietcool::AuthorityLossReason freshness[] = {
      ::quietcool::AuthorityLossReason::ExactOemQuery,
      ::quietcool::AuthorityLossReason::ConsensusTimeout,
      ::quietcool::AuthorityLossReason::TransactionExhausted,
      ::quietcool::AuthorityLossReason::ManualRevalidationPending,
      ::quietcool::AuthorityLossReason::RadioUnavailable,
  };
  for (const auto reason : freshness) {
    const auto next = confirmed_state_after(snapshot_with_unknown(reason), high);
    QC_CHECK(next.has_value());
  }
}

QC_TEST("timer_command", "external state traffic replaces the cache with the observed state") {
  // ExternalStateTraffic is raised by HandleExternalState, which records the
  // triggering frame into last_diagnostic IMMEDIATELY before invalidating —
  // the one loss reason whose diagnostic is fresh by construction (round 4,
  // all three engines traced the single writer). Keeping the stale confirmed
  // value here made the timer select override an OEM remote press: someone
  // turns the fan LOW downstairs, someone upstairs picks "4 hours", and the
  // fan jumps back to HIGH from an entity nobody used to change speed.
  const auto high = FanState::command(Speed::High, Duration::Continuous);
  const auto observed_low =
      FanState::command(Speed::Low, Duration::Continuous);
  auto snapshot = snapshot_with_unknown(
      ::quietcool::AuthorityLossReason::ExternalStateTraffic);
  std::get<::quietcool::UnknownStateAuthority>(snapshot.state)
      .last_diagnostic = observed_low;
  const auto next = confirmed_state_after(snapshot, high);
  QC_CHECK(next.has_value());
  QC_CHECK_EQ(next->canonical_byte(), observed_low.canonical_byte());
}

QC_TEST("timer_command", "external state traffic without an observed state clears the cache") {
  // Defensive only — ExternalStateTraffic cannot actually arrive without a
  // recorded diagnostic — but if it ever did, the known-contradicted confirmed
  // value must not survive; empty cache is the stopped-fan LOW rule.
  const auto high = FanState::command(Speed::High, Duration::Continuous);
  const auto next = confirmed_state_after(
      snapshot_with_unknown(
          ::quietcool::AuthorityLossReason::ExternalStateTraffic),
      high);
  QC_CHECK(!next.has_value());
}

QC_TEST("timer_command", "ambiguous yield and tail contradiction keep the cache") {
  // These two also carry contrary evidence — but the core never RECORDS it
  // (record_diagnostic has exactly one call site, HandleExternalState), so at
  // this layer last_diagnostic is absent or arbitrarily stale for them. Wave 3
  // put them on the diagnostic-fallback branch anyway, and round 4 MEASURED
  // the consequence: a single contradicting tail frame — the documented
  // stale-echo signature, often the fan's own echo — cleared the cache and
  // turned a confirmed-HIGH fan's "4 hours" into LOW+4h; a stale diagnostic
  // silently substituted a 12-hour-old observation for a newer confirmed
  // state. Keeping the last CONFIRMED value is the least-wrong option this
  // layer can implement; recording the evidence at the two raise sites would
  // be a core change this branch deliberately excludes.
  const auto high = FanState::command(Speed::High, Duration::Continuous);
  const auto stale_low = FanState::command(Speed::Low, Duration::Continuous);
  const ::quietcool::AuthorityLossReason unrecorded[] = {
      ::quietcool::AuthorityLossReason::AmbiguousOemYield,
      ::quietcool::AuthorityLossReason::ContradictoryTail,
  };
  for (const auto reason : unrecorded) {
    // Even with a (necessarily stale) diagnostic present, keep the confirmed
    // value — the diagnostic's age is unknowable here.
    auto snapshot = snapshot_with_unknown(reason);
    std::get<::quietcool::UnknownStateAuthority>(snapshot.state)
        .last_diagnostic = stale_low;
    const auto next = confirmed_state_after(snapshot, high);
    QC_CHECK(next.has_value());
    QC_CHECK_EQ(next->canonical_byte(), high.canonical_byte());
  }
}

QC_TEST("timer_command", "identity invalidations clear the cache") {
  // These change WHAT the cached state describes: a different fan (Forget /
  // Learn), no fan at all, or a timer deadline after which the fan is presumed
  // stopped. Keeping the cache through these was round 1's high finding — a
  // re-bound or expired-timer fan started at the previous state's speed.
  const auto high = FanState::command(Speed::High, Duration::Continuous);
  const ::quietcool::AuthorityLossReason identity[] = {
      ::quietcool::AuthorityLossReason::Unprovisioned,
      ::quietcool::AuthorityLossReason::SenderChanged,
      ::quietcool::AuthorityLossReason::LearningStarted,
      ::quietcool::AuthorityLossReason::EstimatedTimerDeadline,
  };
  for (const auto reason : identity) {
    const auto next = confirmed_state_after(snapshot_with_unknown(reason), high);
    QC_CHECK(!next.has_value());
  }
}

QC_TEST("timer_command", "revalidation keeps the cache") {
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.state = ::quietcool::RevalidatingStateAuthority{};
  const auto high = FanState::command(Speed::High, Duration::Continuous);
  const auto next = confirmed_state_after(snapshot, high);
  QC_CHECK(next.has_value());
}

QC_TEST("timer_command", "apply snapshot caches capability and confirmed state") {
  TimerSelectCache cache;
  ::quietcool::AuthoritySnapshot snapshot{};
  const auto high = FanState::command(Speed::High, Duration::Continuous);
  snapshot.state = ::quietcool::ConfirmedStateAuthority{
      high,
      ::quietcool::EvidenceSource::ManualQueryConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      0,
      2,
      std::nullopt,
      std::nullopt,
      1};
  snapshot.speed_capability = ::quietcool::SpeedCapability::Three;
  snapshot.timer = ::quietcool::NoActiveTimerAuthority{};

  const auto option = timer_select_apply_snapshot(cache, snapshot, "");

  QC_CHECK(cache.confirmed.has_value());
  QC_CHECK_EQ(cache.confirmed->canonical_byte(), high.canonical_byte());
  QC_CHECK(cache.capability.has_value());
  QC_CHECK(option.has_value());
  QC_CHECK(std::string(option.value()) == "None");
}

QC_TEST("timer_command", "apply snapshot suppresses a republish of the shown option") {
  // Snapshots arrive every effect-drain round — essentially every loop tick —
  // so an undeduped select would stream one native-API message per tick
  // (round 3, opus, measured at 50 publishes in 50 idle ticks on the
  // undeduped diagnostics). Same option in, nothing out.
  TimerSelectCache cache;
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::NoActiveTimerAuthority{};
  QC_CHECK(!timer_select_apply_snapshot(cache, snapshot, "None").has_value());
  QC_CHECK(timer_select_apply_snapshot(cache, snapshot, "2 hours").has_value());
  // No shown option yet -> publish.
  QC_CHECK(timer_select_apply_snapshot(cache, snapshot, nullptr).has_value());
}

QC_TEST("timer_command", "apply snapshot clears the cache on a re-binding") {
  TimerSelectCache cache;
  cache.confirmed = FanState::command(Speed::High, Duration::Continuous);
  const auto snapshot = snapshot_with_unknown(
      ::quietcool::AuthorityLossReason::SenderChanged);
  timer_select_apply_snapshot(cache, snapshot, "");
  QC_CHECK(!cache.confirmed.has_value());
}

QC_TEST("timer_command", "a confirmed off duration never publishes a stop") {
  // Duration::Off should not reach a timer authority, but if it ever does the
  // select must fall back to "None" — the option list has no way to express a
  // stopped fan, and inventing one would put Off back into the timer path.
  ::quietcool::AuthoritySnapshot snapshot{};
  ::quietcool::ProgrammedDurationAuthority programmed{};
  programmed.duration = ::quietcool::Duration::Off;
  snapshot.timer = programmed;
  QC_CHECK(std::string(timer_option_for_authority(snapshot).value()) == "None");
}

QC_TEST("timer_command", "a press composes from confirmed state while a timer is not due") {
  TimerSelectCache cache;
  cache.confirmed = FanState::command(Speed::High, Duration::Hours1);
  cache.capability = ::quietcool::SpeedCapability::Three;
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::LocallyAnchoredTimerAuthority{
      ::quietcool::Duration::Hours1,
      ::quietcool::TransactionId(1),
      ::quietcool::AttemptNumber(1),
      0,
      3600000,  // expires at t=1h
      ::quietcool::EvidenceSource::PostCommandConsensus};
  const auto command = timer_command_for_press(
      cache, snapshot, 1800000 /* t=30min */, TimerSelection::Continuous);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xBF);
}

QC_TEST("timer_command", "a press after the estimated deadline takes the stopped fan rule") {
  // Round 6 (codex): the estimated deadline is processed by poll(), so a
  // press landing in the tick where it is due-but-unprocessed still sees the
  // confirmed running state in the snapshot — and "None" would then transmit
  // a HIGH restart at the exact moment the fan is presumed to have stopped.
  // A due estimate means the confirmed state is no longer trustworthy for
  // composition: fall back to the stopped-fan LOW rule.
  TimerSelectCache cache;
  cache.confirmed = FanState::command(Speed::High, Duration::Hours1);
  cache.capability = ::quietcool::SpeedCapability::Three;
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::LocallyAnchoredTimerAuthority{
      ::quietcool::Duration::Hours1,
      ::quietcool::TransactionId(1),
      ::quietcool::AttemptNumber(1),
      0,
      3600000,
      ::quietcool::EvidenceSource::PostCommandConsensus};
  const auto command = timer_command_for_press(
      cache, snapshot, 3600001 /* just past expiry */, TimerSelection::Continuous);
  QC_CHECK_EQ(command.outbound_command_byte(), 0x9F);
  QC_CHECK_EQ(command.speed().value(), Speed::Low);
}

QC_TEST("timer_command", "a press at the exact anchored deadline is already due") {
  // Pins the >= boundary (round 7, opus): weakening the predicate to > was
  // previously invisible because only expiry+1 was probed.
  TimerSelectCache cache;
  cache.confirmed = FanState::command(Speed::High, Duration::Hours1);
  cache.capability = ::quietcool::SpeedCapability::Three;
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::LocallyAnchoredTimerAuthority{
      ::quietcool::Duration::Hours1,
      ::quietcool::TransactionId(1),
      ::quietcool::AttemptNumber(1),
      0,
      3600000,
      ::quietcool::EvidenceSource::PostCommandConsensus};
  const auto command = timer_command_for_press(
      cache, snapshot, 3600000 /* exactly the deadline */,
      TimerSelection::Continuous);
  QC_CHECK_EQ(command.speed().value(), Speed::Low);
}

QC_TEST("timer_command", "an expired programmed duration takes the stopped fan rule") {
  // Round 7 (opus, probe-proven MEDIUM): an OEM-remote-set or boot-query-
  // observed timer lands in ProgrammedDurationAuthority, which carries no
  // deadline anywhere in the system — the core cannot estimate its expiry
  // (start time unknown), so EstimatedTimerDeadline never fires for it and
  // the fan stops on its own with nothing noticing. The observation time
  // bounds the expiry from above: a Hours4 timer observed at t0 cannot still
  // be running at t0+4h, so a press after that composes from the stopped-fan
  // LOW rule rather than restarting the stopped fan at its old speed.
  TimerSelectCache cache;
  cache.confirmed = FanState::command(Speed::High, Duration::Hours4);
  cache.capability = ::quietcool::SpeedCapability::Three;
  ::quietcool::AuthoritySnapshot snapshot{};
  ::quietcool::ProgrammedDurationAuthority programmed{};
  programmed.duration = ::quietcool::Duration::Hours4;
  programmed.observed_ms = 0;
  snapshot.timer = programmed;
  const auto command = timer_command_for_press(
      cache, snapshot, 5 * 3600000 /* 5h after observation */,
      TimerSelection::Continuous);
  QC_CHECK_EQ(command.outbound_command_byte(), 0x9F);
  QC_CHECK_EQ(command.speed().value(), Speed::Low);
}

QC_TEST("timer_command", "the programmed duration boundary is exact for every timed duration") {
  // Table-driven and boundary-exact (round 8, opus): the earlier probes sat
  // 45+ minutes from the boundary, so a 2.8% error in the hour constant —
  // ~10 minutes on a 4h timer — survived, and only Hours4 was exercised at
  // all. duration_run_ms is deliberately a local copy of the core's
  // anonymous-namespace duration_ms (exporting it would be a core change);
  // this table is what pins the two to the same arithmetic.
  const struct { ::quietcool::Duration duration; std::uint64_t hours; } cases[] = {
      {::quietcool::Duration::Hours1, 1},  {::quietcool::Duration::Hours2, 2},
      {::quietcool::Duration::Hours4, 4},  {::quietcool::Duration::Hours8, 8},
      {::quietcool::Duration::Hours12, 12},
  };
  for (const auto& c : cases) {
    const std::uint64_t observed = 500;
    const std::uint64_t expiry = observed + c.hours * 3600000ULL;
    ::quietcool::AuthoritySnapshot snapshot{};
    ::quietcool::ProgrammedDurationAuthority programmed{};
    programmed.duration = c.duration;
    programmed.observed_ms = observed;
    snapshot.timer = programmed;
    // One millisecond before the bound: still composes from confirmed HIGH.
    TimerSelectCache before;
    before.confirmed = FanState::command(Speed::High, Duration::Continuous);
    before.capability = ::quietcool::SpeedCapability::Three;
    QC_CHECK_EQ(timer_command_for_press(before, snapshot, expiry - 1,
                                        TimerSelection::Continuous)
                    .outbound_command_byte(),
                0xBF);
    // Exactly the bound: presumed stopped, LOW rule (pins >=, not >).
    TimerSelectCache at;
    at.confirmed = FanState::command(Speed::High, Duration::Continuous);
    at.capability = ::quietcool::SpeedCapability::Three;
    QC_CHECK_EQ(timer_command_for_press(at, snapshot, expiry,
                                        TimerSelection::Continuous)
                    .outbound_command_byte(),
                0x9F);
  }
}

QC_TEST("timer_command", "a programmed duration not yet due composes from confirmed state") {
  TimerSelectCache cache;
  cache.confirmed = FanState::command(Speed::High, Duration::Hours4);
  cache.capability = ::quietcool::SpeedCapability::Three;
  ::quietcool::AuthoritySnapshot snapshot{};
  ::quietcool::ProgrammedDurationAuthority programmed{};
  programmed.duration = ::quietcool::Duration::Hours4;
  programmed.observed_ms = 0;
  snapshot.timer = programmed;
  const auto command = timer_command_for_press(
      cache, snapshot, 3 * 3600000, TimerSelection::Continuous);
  QC_CHECK_EQ(command.outbound_command_byte(), 0xBF);
}

QC_TEST("timer_command", "a due press clears the cached confirmed state") {
  // Round 7 (opus, probe-proven): the presumption must OUTLIVE the press —
  // the press's own request_state invalidates the timer authority, so the
  // evidence the presumption was derived from is destroyed. Without
  // persisting it, only the first press composed correctly and a retry a
  // minute later ran off the pre-expiry speed.
  TimerSelectCache cache;
  cache.confirmed = FanState::command(Speed::High, Duration::Hours1);
  cache.capability = ::quietcool::SpeedCapability::Three;
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::LocallyAnchoredTimerAuthority{
      ::quietcool::Duration::Hours1,
      ::quietcool::TransactionId(1),
      ::quietcool::AttemptNumber(1),
      0,
      3600000,
      ::quietcool::EvidenceSource::PostCommandConsensus};
  (void)timer_command_for_press(cache, snapshot, 3600001,
                                TimerSelection::Continuous);
  QC_CHECK(!cache.confirmed.has_value());
}

namespace {
::quietcool::AuthoritySnapshot snapshot_with_anchored(std::uint64_t expiry_ms) {
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::LocallyAnchoredTimerAuthority{
      ::quietcool::Duration::Hours1,
      ::quietcool::TransactionId(1),
      ::quietcool::AttemptNumber(1),
      0,
      expiry_ms,
      ::quietcool::EvidenceSource::PostCommandConsensus};
  return snapshot;
}
}  // namespace

QC_TEST("timer_command", "an expiry bound survives a freshness timer invalidation") {
  // Round 11 (codex, high): a Refresh or an OEM query can invalidate the
  // TIMER authority to Unknown moments before the deadline, while the speed
  // cache rightly survives (freshness). The deadline knowledge must survive
  // with it, or a press after the real expiry composes from the retained
  // confirmed HIGH and restarts the stopped fan at HIGH.
  TimerSelectCache cache;
  auto anchored = snapshot_with_anchored(3600000);
  anchored.state = ::quietcool::ConfirmedStateAuthority{
      FanState::command(Speed::High, Duration::Hours1),
      ::quietcool::EvidenceSource::PostCommandConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      0, 2, std::nullopt, std::nullopt, 1};
  (void)timer_select_apply_snapshot(cache, anchored, "");

  // Timer authority invalidated (freshness) — the state cache keeps HIGH.
  ::quietcool::AuthoritySnapshot invalidated{};
  invalidated.state = anchored.state;
  invalidated.timer =
      ::quietcool::UnknownTimerAuthority{::quietcool::TimerLossReason::Unknown, 0};
  (void)timer_select_apply_snapshot(cache, invalidated, "");
  QC_CHECK(cache.confirmed.has_value());

  // Press after the remembered deadline: presumed stopped, LOW rule.
  const auto late = timer_command_for_press(cache, invalidated, 3600001,
                                            TimerSelection::Continuous);
  QC_CHECK_EQ(late.outbound_command_byte(), 0x9F);
  QC_CHECK_EQ(late.speed().value(), Speed::Low);
}

QC_TEST("timer_command", "an expiry bound does not fire before its deadline") {
  TimerSelectCache cache;
  (void)timer_select_apply_snapshot(cache, snapshot_with_anchored(3600000), "");
  cache.confirmed = FanState::command(Speed::High, Duration::Hours1);
  cache.capability = ::quietcool::SpeedCapability::Three;
  ::quietcool::AuthoritySnapshot invalidated{};
  invalidated.timer =
      ::quietcool::UnknownTimerAuthority{::quietcool::TimerLossReason::Unknown, 0};
  const auto early = timer_command_for_press(cache, invalidated, 1800000,
                                             TimerSelection::Continuous);
  QC_CHECK_EQ(early.outbound_command_byte(), 0xBF);
}

QC_TEST("timer_command", "confirmed no-active-timer clears the expiry bound") {
  TimerSelectCache cache;
  (void)timer_select_apply_snapshot(cache, snapshot_with_anchored(3600000), "");
  ::quietcool::AuthoritySnapshot cleared{};
  cleared.timer = ::quietcool::NoActiveTimerAuthority{};
  (void)timer_select_apply_snapshot(cache, cleared, "");
  cache.confirmed = FanState::command(Speed::High, Duration::Continuous);
  cache.capability = ::quietcool::SpeedCapability::Three;
  const auto press = timer_command_for_press(cache, cleared, 4000000,
                                             TimerSelection::Continuous);
  QC_CHECK_EQ(press.outbound_command_byte(), 0xBF);
}

QC_TEST("timer_command", "a re-binding clears the previous fan's expiry bound") {
  // Fan A's deadline must not presume fan B stopped: after Forget/Learn and a
  // fresh confirmation, a press past A's old deadline composes from B's
  // confirmed state.
  TimerSelectCache cache;
  (void)timer_select_apply_snapshot(cache, snapshot_with_anchored(3600000), "");
  (void)timer_select_apply_snapshot(
      cache,
      snapshot_with_unknown(::quietcool::AuthorityLossReason::SenderChanged), "");
  ::quietcool::AuthoritySnapshot fresh{};
  fresh.state = ::quietcool::ConfirmedStateAuthority{
      FanState::command(Speed::High, Duration::Continuous),
      ::quietcool::EvidenceSource::ManualQueryConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      0, 2, std::nullopt, std::nullopt, 2};
  fresh.speed_capability = ::quietcool::SpeedCapability::Three;
  (void)timer_select_apply_snapshot(cache, fresh, "");
  const auto press = timer_command_for_press(cache, fresh, 4000000,
                                             TimerSelection::Continuous);
  QC_CHECK_EQ(press.outbound_command_byte(), 0xBF);
}

QC_TEST("timer_command", "an oem frame rebuilds the expiry bound from its own program") {
  // Round 12 (codex + opus, opus probe-proven): wave 11's persisted bound
  // outlived the OEM frame that superseded its program. The cache took the
  // frame's NEW state (HIGH 12h) while the bound kept the OLD deadline — so
  // once that dead deadline passed, a press presumed the remote-set running
  // fan stopped and dropped it to LOW. The bound must be rebuilt from the
  // frame, exactly like the confirmed cache is.
  TimerSelectCache cache;
  auto anchored = snapshot_with_anchored(3600000);
  anchored.state = ::quietcool::ConfirmedStateAuthority{
      FanState::command(Speed::High, Duration::Hours1),
      ::quietcool::EvidenceSource::PostCommandConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      0, 2, std::nullopt, std::nullopt, 1};
  (void)timer_select_apply_snapshot(cache, anchored, "");

  // t=30min: the OEM remote sets HIGH+12h; ExternalStateTraffic records the
  // frame and invalidates both authorities.
  auto oem = snapshot_with_unknown(
      ::quietcool::AuthorityLossReason::ExternalStateTraffic);
  auto& unknown = std::get<::quietcool::UnknownStateAuthority>(oem.state);
  unknown.since_ms = 1800000;
  unknown.last_diagnostic = FanState::command(Speed::High, Duration::Hours12);
  (void)timer_select_apply_snapshot(cache, oem, "");
  QC_CHECK(cache.confirmed.has_value());

  // t=70min: past the DEAD deadline, inside the live one. The press must
  // compose from the remote's HIGH, not the stopped-fan LOW rule.
  const auto press =
      timer_command_for_press(cache, oem, 4200000, TimerSelection::Hours4);
  QC_CHECK_EQ(press.outbound_command_byte(), 0xB4);
  QC_CHECK_EQ(press.speed().value(), Speed::High);

  // Past the REBUILT deadline (since_ms + 12h): presumed stopped, LOW rule.
  const auto late = timer_command_for_press(cache, oem, 1800000 + 43200000 + 1,
                                            TimerSelection::Hours4);
  QC_CHECK_EQ(late.outbound_command_byte(), 0x94);
}

QC_TEST("timer_command", "an oem continuous frame clears the expiry bound") {
  // A continuous program has no deadline; keeping the old one would presume
  // the remote's run-until-stopped fan stopped at the dead timer's expiry.
  TimerSelectCache cache;
  (void)timer_select_apply_snapshot(cache, snapshot_with_anchored(3600000), "");
  auto oem = snapshot_with_unknown(
      ::quietcool::AuthorityLossReason::ExternalStateTraffic);
  auto& unknown = std::get<::quietcool::UnknownStateAuthority>(oem.state);
  unknown.since_ms = 1800000;
  unknown.last_diagnostic = FanState::command(Speed::High, Duration::Continuous);
  (void)timer_select_apply_snapshot(cache, oem, "");
  const auto press = timer_command_for_press(cache, oem, 4000000,
                                             TimerSelection::Continuous);
  QC_CHECK_EQ(press.outbound_command_byte(), 0xBF);
}

QC_TEST("timer_command", "an observation of the running program keeps the tighter bound") {
  // Round 13 (opus, probe-proven): ExternalPriorityState is also raised for
  // frames that merely OBSERVE the program already running (an unsolicited
  // self-report while idle; a query reply inside the 300 ms acceptance
  // start). For those, since_ms is an observation time — rebuilding pushed
  // the bound LATE by the elapsed run and discarded the correct locally
  // anchored deadline, so a post-expiry press restarted the stopped fan at
  // HIGH. An identical-program frame must keep the tighter existing bound.
  TimerSelectCache cache;
  auto anchored = snapshot_with_anchored(3600000);
  anchored.state = ::quietcool::ConfirmedStateAuthority{
      FanState::command(Speed::High, Duration::Hours1),
      ::quietcool::EvidenceSource::PostCommandConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      0, 2, std::nullopt, std::nullopt, 1};
  (void)timer_select_apply_snapshot(cache, anchored, "");

  // t=30min: the fan self-reports the SAME still-running HIGH+1h program.
  auto oem = snapshot_with_unknown(
      ::quietcool::AuthorityLossReason::ExternalStateTraffic);
  auto& unknown = std::get<::quietcool::UnknownStateAuthority>(oem.state);
  unknown.since_ms = 1800000;
  unknown.last_diagnostic = FanState::command(Speed::High, Duration::Hours1);
  (void)timer_select_apply_snapshot(cache, oem, "");

  // t=66min: past the REAL deadline. Presumed stopped, LOW rule — not a
  // HIGH restart off a bound inflated to 1800000 + 1h.
  const auto press =
      timer_command_for_press(cache, oem, 4000000, TimerSelection::Hours4);
  QC_CHECK_EQ(press.outbound_command_byte(), 0x94);
  QC_CHECK_EQ(press.speed().value(), Speed::Low);
}

QC_TEST("timer_command", "an identical program with no bound still rebuilds it") {
  // The keep rule applies only when there is a tighter bound to keep: with
  // none, the frame's own upper bound (since_ms + run) is strictly better
  // than never presuming expiry at all.
  TimerSelectCache cache;
  cache.confirmed = FanState::command(Speed::High, Duration::Hours1);
  cache.capability = ::quietcool::SpeedCapability::Three;
  auto oem = snapshot_with_unknown(
      ::quietcool::AuthorityLossReason::ExternalStateTraffic);
  auto& unknown = std::get<::quietcool::UnknownStateAuthority>(oem.state);
  unknown.since_ms = 1800000;
  unknown.last_diagnostic = FanState::command(Speed::High, Duration::Hours1);
  (void)timer_select_apply_snapshot(cache, oem, "");

  // Inside the rebuilt bound: composes from the observed HIGH.
  const auto press =
      timer_command_for_press(cache, oem, 4000000, TimerSelection::Hours4);
  QC_CHECK_EQ(press.outbound_command_byte(), 0xB4);
  // Past it (1800000 + 1h): presumed stopped, LOW rule.
  const auto late = timer_command_for_press(cache, oem, 5400001,
                                            TimerSelection::Hours4);
  QC_CHECK_EQ(late.outbound_command_byte(), 0x94);
}

QC_TEST("timer_command", "a reconfirmation of the same program keeps the tighter bound") {
  // Round 14 (codex): after a holdoff, the recovery-query consensus
  // re-confirms the UNCHANGED program as ProgrammedDuration with
  // observed_ms = re-confirmation time. Rebuilding from that observation
  // pushed the bound late by the elapsed run and discarded the anchored
  // deadline the wave-13 arm had just preserved — the same defect one
  // variant over.
  TimerSelectCache cache;
  auto anchored = snapshot_with_anchored(3600000);
  anchored.state = ::quietcool::ConfirmedStateAuthority{
      FanState::command(Speed::High, Duration::Hours1),
      ::quietcool::EvidenceSource::PostCommandConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      0, 2, std::nullopt, std::nullopt, 1};
  (void)timer_select_apply_snapshot(cache, anchored, "");

  // t=30min: query consensus re-confirms the same still-running HIGH+1h.
  ::quietcool::AuthoritySnapshot reconfirmed{};
  reconfirmed.state = ::quietcool::ConfirmedStateAuthority{
      FanState::command(Speed::High, Duration::Hours1),
      ::quietcool::EvidenceSource::ManualQueryConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      1800000, 2, std::nullopt, std::nullopt, 2};
  ::quietcool::ProgrammedDurationAuthority programmed{};
  programmed.duration = ::quietcool::Duration::Hours1;
  programmed.observed_ms = 1800000;
  reconfirmed.timer = programmed;
  (void)timer_select_apply_snapshot(cache, reconfirmed, "");

  // t=66min: past the REAL deadline (60min), inside the inflated one
  // (90min). Presumed stopped, LOW rule.
  const auto press = timer_command_for_press(cache, reconfirmed, 4000000,
                                             TimerSelection::Hours4);
  QC_CHECK_EQ(press.outbound_command_byte(), 0x94);
  QC_CHECK_EQ(press.speed().value(), Speed::Low);
}

QC_TEST("timer_command", "a same-program reconfirmation with no bound still sets one") {
  // The keep rule applies only when there is a bound to keep: with none,
  // the observation's own upper bound must land, or a later freshness
  // invalidation leaves no deadline at all and a post-expiry press
  // composes HIGH. (Pressed through an unknown-timer snapshot so the
  // persisted bound — not the snapshot's own programmed variant — is what
  // fires.)
  TimerSelectCache cache;
  cache.confirmed = FanState::command(Speed::High, Duration::Hours1);
  cache.capability = ::quietcool::SpeedCapability::Three;
  ::quietcool::AuthoritySnapshot reconfirmed{};
  reconfirmed.state = ::quietcool::ConfirmedStateAuthority{
      FanState::command(Speed::High, Duration::Hours1),
      ::quietcool::EvidenceSource::ManualQueryConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      1800000, 2, std::nullopt, std::nullopt, 2};
  ::quietcool::ProgrammedDurationAuthority programmed{};
  programmed.duration = ::quietcool::Duration::Hours1;
  programmed.observed_ms = 1800000;
  reconfirmed.timer = programmed;
  (void)timer_select_apply_snapshot(cache, reconfirmed, "");

  ::quietcool::AuthoritySnapshot invalidated{};
  invalidated.state = reconfirmed.state;
  invalidated.timer =
      ::quietcool::UnknownTimerAuthority{::quietcool::TimerLossReason::Unknown, 0};
  (void)timer_select_apply_snapshot(cache, invalidated, "");

  // Past observed + 1h: presumed stopped via the persisted bound.
  const auto press = timer_command_for_press(cache, invalidated, 5400001,
                                             TimerSelection::Hours4);
  QC_CHECK_EQ(press.outbound_command_byte(), 0x94);
}

QC_TEST("timer_command", "a reconfirmation of a different program rebuilds the bound") {
  // The keep rule must not outlive the program it bounds: a NEW program
  // (here 2 hours) replaces the dead deadline with its own upper bound.
  TimerSelectCache cache;
  auto anchored = snapshot_with_anchored(3600000);
  anchored.state = ::quietcool::ConfirmedStateAuthority{
      FanState::command(Speed::High, Duration::Hours1),
      ::quietcool::EvidenceSource::PostCommandConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      0, 2, std::nullopt, std::nullopt, 1};
  (void)timer_select_apply_snapshot(cache, anchored, "");

  ::quietcool::AuthoritySnapshot reconfirmed{};
  reconfirmed.state = ::quietcool::ConfirmedStateAuthority{
      FanState::command(Speed::High, Duration::Hours2),
      ::quietcool::EvidenceSource::ManualQueryConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      1800000, 2, std::nullopt, std::nullopt, 2};
  ::quietcool::ProgrammedDurationAuthority programmed{};
  programmed.duration = ::quietcool::Duration::Hours2;
  programmed.observed_ms = 1800000;
  reconfirmed.timer = programmed;
  (void)timer_select_apply_snapshot(cache, reconfirmed, "");

  // t=66min: past the DEAD 1h deadline, inside the live 2h program's bound
  // (observed 30min + 2h = 2h30min). Composes from confirmed HIGH.
  const auto press = timer_command_for_press(cache, reconfirmed, 4000000,
                                             TimerSelection::Hours4);
  QC_CHECK_EQ(press.outbound_command_byte(), 0xB4);
  QC_CHECK_EQ(press.speed().value(), Speed::High);
}

QC_TEST("timer_command", "a continuous programmed duration clears the expiry bound") {
  // Round 12 (opus): this arm was defensive and unbound — the core's promote
  // routes Off/Continuous to NoActiveTimer today, but nothing pinned the arm
  // that would become load-bearing the day that routing changes. Pin it at
  // this layer: a Continuous program carries no deadline, so a stale bound
  // must not survive it.
  TimerSelectCache cache;
  (void)timer_select_apply_snapshot(cache, snapshot_with_anchored(3600000), "");
  ::quietcool::AuthoritySnapshot continuous{};
  ::quietcool::ProgrammedDurationAuthority programmed{};
  programmed.duration = ::quietcool::Duration::Continuous;
  programmed.observed_ms = 1800000;
  continuous.timer = programmed;
  (void)timer_select_apply_snapshot(cache, continuous, "");
  cache.confirmed = FanState::command(Speed::High, Duration::Continuous);
  cache.capability = ::quietcool::SpeedCapability::Three;
  const auto press = timer_command_for_press(cache, continuous, 4000000,
                                             TimerSelection::Continuous);
  QC_CHECK_EQ(press.outbound_command_byte(), 0xBF);
}

}  // namespace
}  // namespace esphome::quietcool
