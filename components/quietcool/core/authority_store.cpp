#include "authority_store.h"

namespace quietcool {
namespace {

std::optional<MonotonicMs> duration_ms(Duration duration) {
  switch (duration) {
    case Duration::Hours1: case Duration::Hours2: case Duration::Hours4:
    case Duration::Hours8: case Duration::Hours12:
      return static_cast<MonotonicMs>(static_cast<std::uint8_t>(duration)) *
             60U * 60U * 1000U;
    default: return std::nullopt;
  }
}

}  // namespace

bool restorable_state_is_valid(const RestorableState& restored) {
  if (restored.version != kRestoreSchemaVersion) return false;
  switch (restored.seed_policy) {
    case SeedPolicy::AllowCompiledSeed:
    case SeedPolicy::SuppressCompiledSeed:break;
    default:return false;
  }
  if (restored.remembered_speed &&
      !speed_from_value(static_cast<std::uint8_t>(*restored.remembered_speed)))
    return false;
  if (restored.speed_capability) {
    switch (*restored.speed_capability) {
      case SpeedCapability::One: case SpeedCapability::Two:
      case SpeedCapability::Three:break;
      // Unknown is deliberately invalid here: only a CONFIRMED capability is
      // ever persisted, so a stored Unknown means a corrupt or forged record.
      default:return false;
    }
  }
  if (!restored.observation_hint) return true;
  const auto& hint = *restored.observation_hint;
  if (hint.canonical_state > 0x3FU) return false;
  const auto state = FanState::observed(hint.canonical_state);
  if (!state) return false;
  switch (hint.capability) {
    case SpeedCapability::Unknown: case SpeedCapability::One:
    case SpeedCapability::Two: case SpeedCapability::Three:break;
    default:return false;
  }
  const auto speed = state.value().speed();
  return hint.capability == SpeedCapability::Unknown || !speed ||
      static_cast<std::uint8_t>(*speed) <=
          static_cast<std::uint8_t>(hint.capability);
}

AuthorityStore::AuthorityStore()
    : state_(UnknownStateAuthority{AuthorityLossReason::Boot, 0, std::nullopt,
                                   std::nullopt}),
      timer_(UnknownTimerAuthority{TimerLossReason::Unknown, 0}) {}

AuthoritySnapshot AuthorityStore::snapshot(MonotonicMs) const {
  return {state_, timer_, remembered_speed_, confirmed_capability_,
          last_diagnostic_, revision_};
}

std::optional<PriorAuthoritySnapshot> AuthorityStore::capture_prior(
    MonotonicMs now_ms) const {
  const auto* confirmed = std::get_if<ConfirmedStateAuthority>(&state_);
  if (!confirmed) {
    const auto* revalidating = std::get_if<RevalidatingStateAuthority>(&state_);
    return revalidating ? revalidating->previous : std::nullopt;
  }
  if (now_ms < confirmed->observed_ms) return std::nullopt;
  return PriorAuthoritySnapshot{confirmed->state, confirmed->source,
                                confirmed->observed_ms, true,
                                confirmed->revision};
}

void AuthorityStore::begin_local_command(TransactionId, MonotonicMs now_ms) {
  set_unknown(AuthorityLossReason::LocalCommandPending,
              TimerLossReason::StateInvalidated, now_ms);
}

void AuthorityStore::begin_manual_revalidation(MonotonicMs now_ms) {
  state_ = RevalidatingStateAuthority{capture_prior(now_ms), now_ms, std::nullopt};
  timer_ = UnknownTimerAuthority{TimerLossReason::StateInvalidated, now_ms};
  ++revision_;
}

void AuthorityStore::bind_manual_query_token(TxToken token) {
  if (auto* value = std::get_if<RevalidatingStateAuthority>(&state_)) value->token = token;
}

void AuthorityStore::promote(const AcceptedObservation& accepted,
                             MonotonicMs now_ms) {
  ++revision_;
  // ConfirmedStateAuthority denotes observational corroboration, not
  // authenticity; the OEM protocol carries no way to prove origin. See SECURITY.md.
  state_ = ConfirmedStateAuthority{
      accepted.state, accepted.source, accepted.confidence,
      accepted.observed_ms, accepted.independent_candidates,
      accepted.transaction, accepted.attempt, revision_};
  if (accepted.state.speed()) remembered_speed_ = accepted.state.speed();
  // Sticky: overwritten by every confirmed report that carries a capability,
  // never cleared by invalidate() — the capability describes the bound fan,
  // not the freshness of the current authority claim (issue #31). It is
  // cleared only through clear_confirmed_capability(), when the binding
  // itself changes (Forget, or learning a different fan).
  //
  // Evidence that could be our own echo (marker bits 10, byte-identical to the
  // frame we just transmitted) is admitted only into an EMPTY slot. That is
  // what lets a 2-speed fan's confirming report — indistinguishable from an
  // echo by construction — teach the band on a fresh unit, while making it
  // impossible for an echo to demote a fan whose capability is already known
  // from an unambiguous frame (issue #31 review). If a fresh unit ever does
  // mis-learn Two from a pure echo, the next unambiguous report (any query
  // reply, or the fan's answer to the re-aimed command) overwrites it.
  if (accepted.capability != SpeedCapability::Unknown &&
      (accepted.capability_evidence == CapabilityEvidence::Unambiguous ||
       !confirmed_capability_))
    confirmed_capability_ = accepted.capability;
  const auto duration = accepted.state.duration();
  const auto duration_anchor = duration_ms(duration);
  if (duration == Duration::Off || duration == Duration::Continuous) {
    timer_ = NoActiveTimerAuthority{duration == Duration::Off, accepted.source,
                                    accepted.observed_ms};
  } else if (duration_anchor && accepted.anchors_local_timer &&
             accepted.command_completed_ms && accepted.transaction &&
             accepted.attempt) {
    timer_ = LocallyAnchoredTimerAuthority{
        duration, *accepted.transaction, *accepted.attempt,
        *accepted.command_completed_ms,
        saturating_add(*accepted.command_completed_ms, *duration_anchor),
        accepted.source, TimerAnchorPolicy::CommandBurstCompletion};
  } else {
    // A malformed duration nibble (never produced by FanState::observed today)
    // falls back to a programmed-duration timer instead of dereferencing a
    // nullopt anchor.
    timer_ = ProgrammedDurationAuthority{duration, accepted.source,
                                         accepted.observed_ms};
  }
  (void) now_ms;
}

void AuthorityStore::invalidate(AuthorityLossReason reason, MonotonicMs now_ms) {
  set_unknown(reason, TimerLossReason::StateInvalidated, now_ms);
}

void AuthorityStore::clear_confirmed_capability() {
  confirmed_capability_.reset();
}

void AuthorityStore::record_diagnostic(FanState state) { last_diagnostic_ = state; }

TimerExpiryDecision AuthorityStore::timer_estimate_expired(MonotonicMs now_ms) {
  const auto deadline = timer_deadline();
  if (!deadline || now_ms < *deadline)
    return {TimerExpiryStatus::NotDue, std::nullopt};
  set_unknown(AuthorityLossReason::EstimatedTimerDeadline,
              TimerLossReason::EstimatedDeadline, now_ms);
  return {TimerExpiryStatus::Due, std::nullopt};
}

std::optional<MonotonicMs> AuthorityStore::timer_deadline() const {
  const auto* local = std::get_if<LocallyAnchoredTimerAuthority>(&timer_);
  return local ? std::optional<MonotonicMs>(local->expiry_ms) : std::nullopt;
}

void AuthorityStore::restore_hint(const RestorableState& restored,
                                  MonotonicMs now_ms) {
  last_diagnostic_.reset();
  remembered_speed_ = restored.remembered_speed;
  confirmed_capability_ = restored.speed_capability;
  restored_hint_ = restored.observation_hint;
  ++revision_;
  state_ = UnknownStateAuthority{AuthorityLossReason::RestoredUnverified,
      now_ms, last_diagnostic_, restored_hint_};
  timer_ = UnknownTimerAuthority{TimerLossReason::RestoredUnverified, now_ms};
}

void AuthorityStore::set_unknown(AuthorityLossReason reason,
                                 TimerLossReason timer_reason,
                                 MonotonicMs now_ms) {
  ++revision_;
  state_ = UnknownStateAuthority{reason, now_ms, last_diagnostic_, restored_hint_};
  timer_ = UnknownTimerAuthority{timer_reason, now_ms};
}

}  // namespace quietcool
