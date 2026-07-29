#include "timer_command.h"

#include "fan_command.h"
#include "fan_feedback.h"

namespace esphome::quietcool {

::quietcool::Duration duration_for_selection(TimerSelection selection) {
  switch (selection) {
    case TimerSelection::Hours1:  return ::quietcool::Duration::Hours1;
    case TimerSelection::Hours2:  return ::quietcool::Duration::Hours2;
    case TimerSelection::Hours4:  return ::quietcool::Duration::Hours4;
    case TimerSelection::Hours8:  return ::quietcool::Duration::Hours8;
    case TimerSelection::Hours12: return ::quietcool::Duration::Hours12;
    case TimerSelection::Continuous: break;
  }
  // Continuous is the default rather than a case so that adding a selection
  // without extending this switch keeps the fan RUNNING. Failing toward
  // Continuous is the safe direction; failing toward Off would stop the fan.
  return ::quietcool::Duration::Continuous;
}

::quietcool::FanState timer_command_from_intent(TimerSelection requested, bool fan_on,
                                                int level,
                                                std::uint8_t command_speed_count) {
  const auto speed = fan_on ? speed_for_level(level, command_speed_count)
                            : ::quietcool::Speed::Low;
  return ::quietcool::FanState::command(speed, duration_for_selection(requested));
}

namespace {

// The option index and the TimerSelection enumerator are the same ordinal, so
// the two directions cannot drift apart as long as both go through here.
constexpr TimerSelection kSelectionByIndex[] = {
    TimerSelection::Continuous, TimerSelection::Hours1, TimerSelection::Hours2,
    TimerSelection::Hours4,     TimerSelection::Hours8, TimerSelection::Hours12};

static_assert(sizeof(kSelectionByIndex) / sizeof(kSelectionByIndex[0]) ==
                  sizeof(kTimerOptions) / sizeof(kTimerOptions[0]),
              "every option string must have exactly one selection");

// The selection for a wire duration. Off degrades to Continuous ("None")
// rather than gaining an option of its own: the select's vocabulary has no
// stop, by design.
TimerSelection selection_for_duration(::quietcool::Duration duration) {
  switch (duration) {
    case ::quietcool::Duration::Hours1:  return TimerSelection::Hours1;
    case ::quietcool::Duration::Hours2:  return TimerSelection::Hours2;
    case ::quietcool::Duration::Hours4:  return TimerSelection::Hours4;
    case ::quietcool::Duration::Hours8:  return TimerSelection::Hours8;
    case ::quietcool::Duration::Hours12: return TimerSelection::Hours12;
    case ::quietcool::Duration::Continuous:
    case ::quietcool::Duration::Off:
      break;
  }
  return TimerSelection::Continuous;
}

// The option for a wire duration, DERIVED from kTimerOptions via the selection
// ordinal rather than restated as string literals. A second list of literals
// here is the round-1 opus finding: rename an option in both select.py and
// kTimerOptions -- the exact edit the codegen-drift test blesses -- and the
// stale literal would still be published, which Select::publish_state refuses
// (no such option), leaving the entity permanently stateless in Home Assistant
// while it keeps transmitting.
const char* option_for_duration(::quietcool::Duration duration) {
  return option_for_selection(selection_for_duration(duration));
}

}  // namespace

std::optional<TimerSelection> selection_for_option(const std::string& option) {
  for (std::size_t index = 0;
       index < sizeof(kTimerOptions) / sizeof(kTimerOptions[0]); ++index) {
    if (option == kTimerOptions[index]) return kSelectionByIndex[index];
  }
  return std::nullopt;
}

const char* option_for_selection(TimerSelection selection) {
  return kTimerOptions[static_cast<std::size_t>(selection)];
}

::quietcool::FanState timer_command_from_confirmed(
    const std::optional<::quietcool::FanState>& confirmed,
    std::optional<::quietcool::SpeedCapability> capability,
    TimerSelection requested) {
  // The band pair is asymmetric on purpose: the confirmed level is read against
  // the ENTITY band (the count the level was published in), while the outgoing
  // nibble is formed against the COMMAND band, which is 2 while capability is
  // unknown and therefore structurally cannot form MED (issues #30, #31). A
  // stale confirmed MED under an unknown capability clamps to HIGH -- one press
  // in the safe direction, never a stop. Do not "simplify" this to one band.
  const auto feedback =
      confirmed ? authority_to_feedback(*confirmed, entity_speed_count(capability))
                : FanFeedback{};
  return timer_command_from_intent(requested, feedback.on,
                                   feedback.speed.value_or(1),
                                   command_speed_count(capability));
}

std::optional<::quietcool::FanState> confirmed_state_after(
    const ::quietcool::AuthoritySnapshot& authority,
    const std::optional<::quietcool::FanState>& previous) {
  if (const auto* confirmed =
          std::get_if<::quietcool::ConfirmedStateAuthority>(&authority.state))
    return confirmed->state;
  if (std::get_if<::quietcool::RevalidatingStateAuthority>(&authority.state))
    return previous;
  const auto* unknown =
      std::get_if<::quietcool::UnknownStateAuthority>(&authority.state);
  // Defensive rather than std::get (round 3, opus): a fourth StateAuthority
  // alternative would make an unguarded get std::terminate on the target
  // (exceptions off) — a reboot loop — where returning "no cache" merely
  // costs a LOW start. The reason switch below stays -Wswitch-protected.
  if (unknown == nullptr) return std::nullopt;
  switch (unknown->reason) {
    // WHAT the cached state describes has changed: a different fan, no fan,
    // or a deadline after which the fan is presumed stopped. Keeping the
    // cache through these aimed a re-bound or expired-timer fan at the
    // previous state's speed (round 1, codex + opus).
    case ::quietcool::AuthorityLossReason::Unprovisioned:
    case ::quietcool::AuthorityLossReason::SenderChanged:
    case ::quietcool::AuthorityLossReason::LearningStarted:
    case ::quietcool::AuthorityLossReason::EstimatedTimerDeadline:
      return std::nullopt;
    // CONTRARY EVIDENCE, RECORDED (rounds 3-4): ExternalStateTraffic is
    // raised by HandleExternalState, which writes the triggering frame into
    // last_diagnostic immediately first — record_diagnostic's ONLY call site
    // in the repo — so the fallback is fresh by construction. A timer pressed
    // during the OEM holdoff then runs the fan at the speed the remote just
    // chose rather than silently overriding it.
    //
    // AmbiguousOemYield and ContradictoryTail are deliberately NOT here,
    // though they also carry contrary evidence: the core never records
    // theirs, so at this layer last_diagnostic is absent or arbitrarily stale
    // for them. Round 4 measured what trusting it did — a single
    // contradicting tail frame (the documented stale-echo signature, often
    // the fan's own echo) turned a confirmed-HIGH fan's "4 hours" into
    // LOW+4h, and a stale diagnostic silently substituted a 12-hour-old
    // observation for a newer confirmed state. They stay on the keep side
    // below; recording their evidence at the raise sites would be a core
    // change this branch deliberately excludes.
    case ::quietcool::AuthorityLossReason::ExternalStateTraffic:
      return unknown->last_diagnostic;
    // Freshness only: we are momentarily unsure, but nothing claims the fan
    // moved. Clearing on these turned the primary "set HIGH, then set a
    // timer" journey into LOW+duration (round 2, opus): begin_local_command
    // raises LocalCommandPending on EVERY accepted command. Boot and
    // RestoredUnverified are vacuous here — they can only precede the first
    // confirmation of a power cycle, when the cache is empty anyway — and
    // grouped with the keep side so a future reordering cannot make them
    // destructive.
    case ::quietcool::AuthorityLossReason::Boot:
    case ::quietcool::AuthorityLossReason::LocalCommandPending:
    case ::quietcool::AuthorityLossReason::ManualRevalidationPending:
    case ::quietcool::AuthorityLossReason::ExactOemQuery:
    case ::quietcool::AuthorityLossReason::AmbiguousOemYield:
    case ::quietcool::AuthorityLossReason::ContradictoryTail:
    case ::quietcool::AuthorityLossReason::ConsensusTimeout:
    case ::quietcool::AuthorityLossReason::TransactionExhausted:
    case ::quietcool::AuthorityLossReason::RadioUnavailable:
    case ::quietcool::AuthorityLossReason::RestoredUnverified:
      break;
  }
  return previous;
}

namespace {

// Milliseconds a timed duration runs for. The timed enumerators' values ARE
// their hour counts (fan_state.h); Off and Continuous have no runtime bound.
std::optional<::quietcool::MonotonicMs> duration_run_ms(
    ::quietcool::Duration duration) {
  switch (duration) {
    case ::quietcool::Duration::Hours1:
    case ::quietcool::Duration::Hours2:
    case ::quietcool::Duration::Hours4:
    case ::quietcool::Duration::Hours8:
    case ::quietcool::Duration::Hours12:
      return static_cast<::quietcool::MonotonicMs>(
                 static_cast<std::uint8_t>(duration)) *
             3600000ULL;
    case ::quietcool::Duration::Off:
    case ::quietcool::Duration::Continuous:
      break;
  }
  return std::nullopt;
}

}  // namespace

std::optional<const char*> timer_select_apply_snapshot(
    TimerSelectCache& cache, const ::quietcool::AuthoritySnapshot& authority,
    const char* shown_option) {
  // Capability first and unconditionally: it is sticky (a property of the
  // bound fan, issue #31), while the confirmed state obeys the reason-
  // discriminating rule above.
  cache.capability = authority.speed_capability;
  // Captured before the update below: the ExternalStateTraffic arm must
  // compare the frame against what the cache believed BEFORE taking it.
  const auto previous_confirmed = cache.confirmed;
  cache.confirmed = confirmed_state_after(authority, cache.confirmed);
  // The expiry bound follows the timer authority when one is visible and
  // SURVIVES the unknown-timer variants (freshness); see its declaration.
  if (const auto* anchored =
          std::get_if<::quietcool::LocallyAnchoredTimerAuthority>(
              &authority.timer)) {
    cache.expiry_bound = anchored->expiry_ms;
  } else if (const auto* programmed =
                 std::get_if<::quietcool::ProgrammedDurationAuthority>(
                     &authority.timer)) {
    if (const auto run_ms = duration_run_ms(programmed->duration)) {
      // Same-program keep rule as the ExternalStateTraffic arm below, for
      // the same reason (round 14, codex): a recovery-query consensus after
      // a holdoff re-confirms the UNCHANGED program as ProgrammedDuration
      // with observed_ms = re-confirmation time — an observation, not a
      // start. Unconditionally rebuilding pushed the bound late by the
      // elapsed run and discarded the tighter locally anchored deadline the
      // wave-13 arm had just preserved, reopening the HIGH-restart-of-a-
      // stopped-fan window one variant over. Same residual, same choice: a
      // genuine same-program re-press keeps a too-early bound and a press
      // drops a running fan to LOW, the preferred error.
      const bool same_program =
          previous_confirmed && cache.confirmed &&
          previous_confirmed->canonical_byte() ==
              cache.confirmed->canonical_byte();
      if (!(same_program && cache.expiry_bound))
        cache.expiry_bound = programmed->observed_ms + *run_ms;
    } else {
      cache.expiry_bound.reset();
    }
  } else if (std::get_if<::quietcool::NoActiveTimerAuthority>(&authority.timer)) {
    cache.expiry_bound.reset();
  }
  if (const auto* unknown =
          std::get_if<::quietcool::UnknownStateAuthority>(&authority.state)) {
    switch (unknown->reason) {
      case ::quietcool::AuthorityLossReason::Unprovisioned:
      case ::quietcool::AuthorityLossReason::SenderChanged:
      case ::quietcool::AuthorityLossReason::LearningStarted:
        cache.expiry_bound.reset();
        break;
      // The OEM remote just changed this fan's timer, so the persisted
      // deadline describes a program that no longer exists. Keeping it while
      // the cache takes the frame's NEW state (the rounds 3-4 rule above)
      // split the cache against itself: once the OLD deadline passed, every
      // press presumed a remote-set running fan stopped and dropped it to LOW
      // (round 12, codex + opus, opus probe-proven). The frame is the single
      // fresh writer of last_diagnostic, so REBUILD the bound from it — a
      // timed program observed at since_ms cannot outlive since_ms + its run
      // length (round 7's upper-bound rule); Off, Continuous and an absent
      // diagnostic carry no deadline. Only this reason: the other freshness
      // reasons claim no fan movement, and clearing on them is exactly the
      // near-expiry hole round 11 (codex) closed.
      //
      // ONLY when the frame's program DIFFERS from what the cache believed
      // (round 13, opus, probe-proven): the classifier also raises
      // ExternalPriorityState for frames that merely OBSERVE the program
      // already running — an unsolicited self-report while no local window
      // is open, or a query reply inside the 300 ms acceptance start. For
      // those, since_ms is an observation time, not a start time, and the
      // unconditional wave-12 rebuild pushed the bound LATE by however long
      // the program had already run, discarding a correct locally anchored
      // deadline — a post-expiry press then restarted the stopped fan at
      // HIGH, the round-1/round-7 defect class. An identical-program frame
      // with a live bound therefore KEEPS the tighter bound. Residual,
      // chosen deliberately: an OEM re-press of the SAME program restarts
      // the fan's real timer, and the kept bound is then too early — a
      // press inside that window drops a running fan to LOW, the error this
      // branch has consistently preferred over energizing a stopped fan at
      // its old speed. Without any existing bound the rebuild proceeds even
      // for an identical program: an upper bound is strictly better than
      // none.
      case ::quietcool::AuthorityLossReason::ExternalStateTraffic: {
        const bool same_program =
            previous_confirmed && unknown->last_diagnostic &&
            previous_confirmed->canonical_byte() ==
                unknown->last_diagnostic->canonical_byte();
        if (same_program && cache.expiry_bound) break;
        cache.expiry_bound.reset();
        if (unknown->last_diagnostic) {
          if (const auto run_ms =
                  duration_run_ms(unknown->last_diagnostic->duration()))
            cache.expiry_bound = unknown->since_ms + *run_ms;
        }
        break;
      }
      default:
        break;
    }
  }
  const auto option = timer_option_for_authority(authority);
  if (!option) return std::nullopt;
  if (shown_option != nullptr && shown_option[0] != '\0' &&
      std::string(shown_option) == *option)
    return std::nullopt;
  return option;
}

::quietcool::FanState timer_command_for_press(
    TimerSelectCache& cache,
    const ::quietcool::AuthoritySnapshot& authority,
    ::quietcool::MonotonicMs now_ms, TimerSelection requested) {
  bool timer_ran_out = false;
  if (const auto* anchored =
          std::get_if<::quietcool::LocallyAnchoredTimerAuthority>(
              &authority.timer)) {
    timer_ran_out = now_ms >= anchored->expiry_ms;
  } else if (const auto* programmed =
                 std::get_if<::quietcool::ProgrammedDurationAuthority>(
                     &authority.timer)) {
    // The core cannot estimate this variant's expiry (start time unknown), so
    // it never fires EstimatedTimerDeadline for it — but the OBSERVATION time
    // bounds the expiry from above: a Hours4 timer observed at t0 cannot
    // still be running at t0+4h (round 7, opus, probe-proven).
    if (const auto run_ms = duration_run_ms(programmed->duration))
      timer_ran_out = now_ms >= programmed->observed_ms + *run_ms;
  }
  // The persisted bound covers deadlines the CURRENT variant no longer
  // carries — a freshness invalidation of the timer authority moments before
  // expiry (round 11, codex).
  if (!timer_ran_out && cache.expiry_bound)
    timer_ran_out = now_ms >= *cache.expiry_bound;
  if (timer_ran_out) {
    // Persist the presumption: the press's own request_state is about to
    // invalidate the timer authority it was derived from, and
    // LocalCommandPending / TransactionExhausted keep the cache — so without
    // clearing here, a failed press's retry would compose from the
    // pre-expiry speed (round 7, opus, probe-proven).
    cache.confirmed = std::nullopt;
  }
  return timer_command_from_confirmed(cache.confirmed, cache.capability,
                                      requested);
}

std::optional<const char*> timer_option_for_authority(
    const ::quietcool::AuthoritySnapshot& authority) {
  if (const auto* anchored =
          std::get_if<::quietcool::LocallyAnchoredTimerAuthority>(&authority.timer))
    return option_for_duration(anchored->duration);
  if (const auto* programmed =
          std::get_if<::quietcool::ProgrammedDurationAuthority>(&authority.timer))
    return option_for_duration(programmed->duration);
  if (std::get_if<::quietcool::NoActiveTimerAuthority>(&authority.timer))
    // Derived, not a literal (round 2): a hand-written "None" here would
    // survive a coordinated option rename and publish a string the Select
    // rejects — the same defect option_for_duration just had fixed.
    return option_for_selection(TimerSelection::Continuous);
  // UnknownTimerAuthority: publish nothing. The entity keeps its last confirmed
  // value rather than claiming a fact no evidence supports.
  return std::nullopt;
}

}  // namespace esphome::quietcool
