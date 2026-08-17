#include "confirmation_core.h"

namespace quietcool {

void ConfirmationCore::cancel_passive_observation() {
  if (state_ != CoordinatorState::Idle) return;
  consensus_.reset();
  passive_response_consensus_.reset();
  if (!passive_observation_) return;
  passive_observation_.reset();
  ++passive_epochs_cancelled_or_expired_;
  const auto recovery = recovery_.snapshot();
  if (recovery.cause == RecoveryCause::OemActivity &&
      recovery.phase != RecoveryPhase::Inactive)
    recovery_.cancel();
}

void ConfirmationCore::ensure_oem_activity_recovery(MonotonicMs now_ms) {
  const auto recovery = recovery_.snapshot();
  const bool terminal = recovery.phase == RecoveryPhase::Complete ||
                        recovery.phase == RecoveryPhase::Expired ||
                        recovery_.poll(now_ms).status ==
                            RecoveryDueStatus::Expired;
  if (recovery.cause != RecoveryCause::OemActivity ||
      recovery.phase == RecoveryPhase::Inactive || terminal)
    recovery_.arm_from_oem_activity(now_ms);
}

void ConfirmationCore::abandon_passive_observation(MonotonicMs now_ms,
                                                    CoreEffects& effects) {
  if (state_ != CoordinatorState::Idle) return;
  const auto partial = consensus_.snapshot();
  const auto response = passive_response_consensus_.snapshot();
  consensus_.reset();
  passive_response_consensus_.reset();
  if (!passive_observation_ && !partial.has_group && !response.has_group) return;
  const bool ambiguous = passive_observation_.has_value() || partial.has_group;
  passive_observation_.reset();
  if (ambiguous) ++passive_epochs_cancelled_or_expired_;
  authority_.invalidate(AuthorityLossReason::ExternalStateTraffic, now_ms);
  effects.add(PublishAuthorityEffect{authority_.snapshot(now_ms)});
  ensure_oem_activity_recovery(now_ms);
  state_ = CoordinatorState::RecoveryQuietWait;
  context_ = RecoveryContext{RecoveryCause::OemActivity};
}

void ConfirmationCore::abandon_passive_response(MonotonicMs now_ms,
                                                CoreEffects& effects) {
  const auto response = passive_response_consensus_.snapshot();
  passive_response_consensus_.reset();
  if (!response.has_group || state_ != CoordinatorState::Idle) return;
  authority_.invalidate(AuthorityLossReason::ExternalStateTraffic, now_ms);
  effects.add(PublishAuthorityEffect{authority_.snapshot(now_ms)});
  ensure_oem_activity_recovery(now_ms);
  const auto partial = consensus_.snapshot();
  const bool ambiguous = passive_observation_.has_value() || partial.has_group;
  if (ambiguous) {
    const auto anchor = passive_observation_ ? passive_observation_->opened_ms
                                             : partial.last_candidate_ms;
    const auto age = elapsed_since(now_ms, anchor);
    if (age && *age <= kPassiveReplyAcceptEndMs) return;
    passive_observation_.reset();
    consensus_.reset();
    ++passive_epochs_cancelled_or_expired_;
  }
  state_ = CoordinatorState::RecoveryQuietWait;
  context_ = RecoveryContext{RecoveryCause::OemActivity};
}

void ConfirmationCore::expire_passive_evidence(MonotonicMs now_ms,
                                                CoreEffects& effects) {
  if (state_ != CoordinatorState::Idle) return;
  if (passive_observation_) {
    const auto age = elapsed_since(now_ms, passive_observation_->opened_ms);
    if (!age || *age > kPassiveReplyAcceptEndMs)
      abandon_passive_observation(now_ms, effects);
    return;
  }
  const auto ambiguous = consensus_.snapshot();
  if (ambiguous.has_group) {
    const auto age = elapsed_since(now_ms, ambiguous.last_candidate_ms);
    if (!age || *age > kPassiveReplyAcceptEndMs) {
      abandon_passive_observation(now_ms, effects);
      return;
    }
  }
  const auto response = passive_response_consensus_.snapshot();
  if (response.has_group) {
    const auto age = elapsed_since(now_ms, response.last_candidate_ms);
    if (!age || *age > kPassiveReplyAcceptEndMs)
      abandon_passive_response(now_ms, effects);
  }
}

CoreEffects ConfirmationCore::handle_passive_candidate(
    const RecoveredResponse& candidate, bool response_only,
    MonotonicMs now_ms) {
  CoreEffects effects;
  if (passive_observation_ && !response_only) {
    const auto age = elapsed_since(now_ms, passive_observation_->opened_ms);
    if (!age || *age > kPassiveReplyAcceptEndMs) {
      abandon_passive_observation(now_ms, effects);
      return effects;
    } else if (!candidate.state.semantically_equals(
                   passive_observation_->first_burst.state)) {
      abandon_passive_observation(now_ms, effects);
      return effects;
    } else {
      const auto silence = elapsed_since(
          now_ms, passive_observation_->last_first_burst_frame_ms);
      if (*age < kPassiveReplyAcceptStartMs || !silence ||
          *silence < kPassiveInterBurstSilenceMs) {
        passive_observation_->last_first_burst_frame_ms = now_ms;
        return {};
      }
    }
  }

  if (response_only) {
    const auto response = passive_response_consensus_.snapshot();
    if (response.has_group) {
      const auto age = elapsed_since(now_ms, response.last_candidate_ms);
      if (!age || *age > kPassiveReplyAcceptEndMs) {
        abandon_passive_response(now_ms, effects);
        return effects;
      }
      const auto prior = FanState::observed(response.latest_raw_byte);
      if (prior && !candidate.state.semantically_equals(prior.value())) {
        abandon_passive_response(now_ms, effects);
        return effects;
      }
    }
  }

  auto& tracker =
      response_only ? passive_response_consensus_ : consensus_;
  const auto consensus = tracker.observe(candidate, now_ms);
  if (!consensus) return effects;
  tracker.reset();

  if (!passive_observation_ && !response_only) {
    passive_observation_ =
        PassiveObservationEpoch{*consensus, now_ms, now_ms};
    recovery_.arm_from_oem_activity(now_ms);
    ++passive_observation_epochs_opened_;
    return effects;
  }

  std::uint8_t independent_candidates = consensus->independent_candidates;
  if (passive_observation_) {
    if (consensus->state.semantically_equals(
            passive_observation_->first_burst.state)) {
      const auto first =
          passive_observation_->first_burst.independent_candidates;
      independent_candidates =
          first > static_cast<std::uint8_t>(0xFFU - independent_candidates)
              ? 0xFFU
              : static_cast<std::uint8_t>(first + independent_candidates);
    }
    passive_observation_.reset();
  }
  consensus_.reset();
  passive_response_consensus_.reset();

  promote_authority(
      AcceptedObservation{consensus->state,
                          consensus->capability,
                          consensus->capability_evidence,
                          EvidenceSource::PassiveObservationConsensus,
                          consensus->confidence,
                          now_ms,
                          independent_candidates,
                          {},
                          {},
                          {},
                          false},
      now_ms, effects);
  recovery_.cancel();
  ++passive_confirmations_promoted_;
  return effects;
}

CoreEffects ConfirmationCore::assert_oem_priority(MonotonicMs now_ms,
                                                  bool external_state) {
  CoreEffects effects;
  cancel_passive_observation();
  if (live_tx_)
    effects.add(RevokeTxLease{live_tx_->token});
  live_tx_.reset();
  window_.reset();
  consensus_.reset();
  clear_deferred();
  if (transaction_)
    finish_transaction(external_state
                           ? TransactionOutcome::CancelledByExternalState
                           : TransactionOutcome::CancelledByExactOemQuery,
                       effects);
  authority_.invalidate(external_state
                            ? AuthorityLossReason::ExternalStateTraffic
                            : AuthorityLossReason::ExactOemQuery,
                        now_ms);
  recovery_.arm_from_oem_activity(now_ms);
  state_ = CoordinatorState::OemHoldoff;
  context_ = OemHoldoffContext{now_ms};
  return effects;
}

FanState ConfirmationCore::safe_tail_state() const {
  const auto snap = consensus_.snapshot();
  if (snap.has_group)
    return FanState::observed(snap.latest_raw_byte).value();
  if (transaction_)
    return transaction_->snapshot().requested;
  return FanState::command(Speed::Low, Duration::Off);
}

void ConfirmationCore::promote_authority(const AcceptedObservation &accepted,
                                         MonotonicMs now_ms,
                                         CoreEffects &effects) {
  const auto before = authority_.snapshot(now_ms);
  authority_.promote(accepted, now_ms);
  const auto after = authority_.snapshot(now_ms);
  if (after.remembered_speed != before.remembered_speed)
    effects.add(RequestPersistenceEffect{{PersistenceKind::SaveRememberedSpeed,
                                          std::nullopt, after.remembered_speed,
                                          std::nullopt}});
  // Persist only on change: capability is confirmed by every report, so an
  // unconditional save here would write NVS once per promote (issue #31).
  if (after.speed_capability != before.speed_capability)
    effects.add(RequestPersistenceEffect{{PersistenceKind::SaveSpeedCapability,
                                          std::nullopt, std::nullopt,
                                          after.speed_capability}});
  effects.add(PublishAuthorityEffect{after});
}
CoreEffects ConfirmationCore::apply_consensus(const Consensus &value,
                                              ActionId action,
                                              MonotonicMs now_ms) {
  CoreEffects effects;
  EvidenceSource source = EvidenceSource::RecoveryQueryConsensus;
  if (state_ == CoordinatorState::PostCommandListening)
    source = EvidenceSource::PostCommandConsensus;
  else if (state_ == CoordinatorState::FallbackResponseListening)
    source = EvidenceSource::FallbackQueryConsensus;
  else if (state_ == CoordinatorState::BootResponseListening)
    source = EvidenceSource::BootQueryConsensus;
  else if (const auto* response = std::get_if<QueryResponseContext>(&context_);
           response && response->purpose == QueryPurpose::Heartbeat)
    source = EvidenceSource::HeartbeatQueryConsensus;
  else if (state_ == CoordinatorState::ManualResponseListening)
    source = EvidenceSource::ManualQueryConsensus;
  else if (const auto* response = std::get_if<QueryResponseContext>(&context_);
           response && response->reason == TxReason::TimerExpiryRecoveryQuery)
    source = EvidenceSource::TimerExpiryRecoveryConsensus;
  const ConsensusObservation observation{value.state, value.capability,
                                         value.confidence, source};
  if (!transaction_ ||
      (state_ != CoordinatorState::PostCommandListening &&
       state_ != CoordinatorState::FallbackResponseListening)) {
    AcceptedObservation accepted{value.state,
                                 value.capability,
                                 value.capability_evidence,
                                 source,
                                 value.confidence,
                                 now_ms,
                                 value.independent_candidates,
                                 {},
                                 {},
                                 {},
                                 false};
    promote_authority(accepted, now_ms, effects);
    recovery_.note_consensus();
    recovery_.cancel();
    enter_tail(ReturnIdle{}, value.state);
    return effects;
  }
  auto tx = transaction_->snapshot();
  const bool remains = transaction_->may_emit_another_command();
  bool mismatch_promotion = false;
  bool policy_matches = false;
  if (tx.requested.is_on()) {
    const auto decision = policy_.decide_on(
        observation, {tx.requested, tx.prior_authority, remains});
    policy_matches =
        std::holds_alternative<ConfirmTransactionAndPromote>(decision)
            ? action == ActionId::ConfirmAndPromote
        : std::holds_alternative<YieldToPossibleOem>(decision)
            ? action == ActionId::YieldToOem
        : std::holds_alternative<RetryWithoutPromotion>(decision)
            ? (action == ActionId::RetryWithoutPromotion ||
               action == ActionId::ApplyMismatchWithRetry)
            : action == ActionId::ExhaustMismatch;
    mismatch_promotion =
        std::holds_alternative<ExhaustAndPromoteObservedState>(decision);
  } else {
    const auto decision = policy_.decide_off(
        observation, {tx.requested, tx.prior_authority, remains});
    policy_matches =
        std::holds_alternative<ConfirmTransactionAndPromote>(decision)
            ? action == ActionId::ConfirmAndPromote
        : (std::holds_alternative<RetryAndPromoteObservedState>(decision) ||
           std::holds_alternative<RetryWithoutPromotion>(decision))
            ? action == ActionId::ApplyMismatchWithRetry
            : action == ActionId::ExhaustMismatch;
    mismatch_promotion =
        std::holds_alternative<ExhaustAndPromoteObservedState>(decision);
  }
  if (!policy_matches) {
    // The guard table and ObservationPolicy are two encodings of one decision.
    // They agree over the whole input domain today (see the exhaustive
    // equivalence test), so this branch is unreachable — but it must fail
    // safe rather than fall through, because the consensus inputs that would
    // select a diverging combination are attacker-controllable by RF
    // injection during the listening window.
    //
    // Returning here without enter_tail() used to leave context_ on the
    // previous alternative while the rule table still forced
    // state_ = ResponseTailQuarantine. The next event's
    // std::get<TailQuarantineContext> would then throw, and with
    // -fno-exceptions on ESP-IDF that is std::terminate: a remotely
    // triggerable reboot loop the moment the two encodings ever drift.
    //
    // Resynchronize instead: surrender authority (the observation cannot be
    // trusted when the two encodings disagree about what it means), close the
    // transaction, and enter the tail so state_ and context_ agree.
    effects.add(PublishCoreEvent{
        {CoreEventKind::InvalidInternalEvent, state_, {}, {}, {}}});
    authority_.invalidate(AuthorityLossReason::ConsensusTimeout, now_ms);
    finish_transaction(TransactionOutcome::Exhausted, effects);
    enter_tail(ReturnIdle{}, value.state);
    return effects;
  }
  const auto promote = [&](bool local) {
    AcceptedObservation accepted{value.state,
                                 value.capability,
                                 value.capability_evidence,
                                 source,
                                 value.confidence,
                                 now_ms,
                                 value.independent_candidates,
                                 tx.id,
                                 AttemptNumber(tx.attempts_started),
                                 last_command_completed_ms_,
                                 local};
    promote_authority(accepted, now_ms, effects);
  };
  if (!tx.requested.is_on() && value.state.is_on() && value.state.speed())
    transaction_->reaim_off_to(*value.state.speed());
  if (action == ActionId::ConfirmAndPromote) {
    promote(true);
    finish_transaction(TransactionOutcome::Confirmed, effects);
    enter_tail(ReturnIdle{}, value.state);
  } else if (action == ActionId::YieldToOem) {
    finish_transaction(TransactionOutcome::YieldedToPossibleOemCommand,
                       effects);
    authority_.invalidate(AuthorityLossReason::AmbiguousOemYield, now_ms);
    recovery_.arm_from_oem_activity(now_ms);
    enter_tail(BeginRecoveryQuietWait{}, value.state);
  } else if (action == ActionId::RetryWithoutPromotion)
    enter_tail(BeginRetryForTransaction{tx.id}, value.state);
  else if (action == ActionId::ApplyMismatchWithRetry) {
    if (!tx.requested.is_on() && !value.state.has_outbound_command_marker())
      promote(false);
    enter_tail(BeginRetryForTransaction{tx.id}, value.state);
  } else {
    if (mismatch_promotion)
      promote(false);
    else
      authority_.invalidate(AuthorityLossReason::TransactionExhausted, now_ms);
    finish_transaction(TransactionOutcome::Exhausted, effects);
    enter_tail(ReturnIdle{}, value.state);
  }
  return effects;
}
CoreEffects ConfirmationCore::close_window_as_miss(MonotonicMs now_ms) {
  CoreEffects effects;
  const auto expected = safe_tail_state();
  if (const auto* response = std::get_if<QueryResponseContext>(&context_);
      response && response->purpose == QueryPurpose::Heartbeat) {
    ++heartbeat_misses_;
    enter_tail(ReturnIdle{}, expected);
    return effects;
  }
  if (state_ == CoordinatorState::PostCommandListening && transaction_) {
    const auto tx = transaction_->snapshot();
    state_ = CoordinatorState::PostCommandTailWait;
    context_ = PostCommandTailContext{tx.id, AttemptNumber(tx.attempts_started),
                                      epoch_identity_};
    return effects;
  }
  if (state_ == CoordinatorState::FallbackResponseListening && transaction_) {
    const auto tx = transaction_->snapshot();
    if (transaction_->may_emit_another_command())
      enter_tail(BeginRetryForTransaction{tx.id}, expected);
    else {
      finish_transaction(TransactionOutcome::Exhausted, effects);
      authority_.invalidate(AuthorityLossReason::TransactionExhausted, now_ms);
      enter_tail(ReturnIdle{}, expected);
    }
    return effects;
  }
  if (state_ == CoordinatorState::RecoveryResponseListening) {
    recovery_.note_empty_window(window_->anchor_ms());
    if (recovery_.snapshot().phase == RecoveryPhase::RetryWait)
      enter_tail(BeginRecoveryRetryWait{}, expected);
    else
      enter_tail(ReturnIdle{}, expected);
    return effects;
  }
  authority_.invalidate(AuthorityLossReason::ConsensusTimeout, now_ms);
  enter_tail(ReturnIdle{}, expected);
  return effects;
}
CoreEffects ConfirmationCore::expire_tail(MonotonicMs now_ms) {
  if (state_ == CoordinatorState::PostCommandTailWait && transaction_) {
    const auto tx = transaction_->snapshot();
    window_.reset();
    consensus_.reset();
    state_ = CoordinatorState::FallbackQueryPending;
    context_ = FallbackQueryContext{tx.id, AttemptNumber(tx.attempts_started)};
    return {};
  }
  if (state_ != CoordinatorState::ResponseTailQuarantine)
    return {};
  const auto exit = std::get<TailQuarantineContext>(context_).exit;
  window_.reset();
  consensus_.reset();
  if (std::holds_alternative<BeginDeferredCommand>(exit) && deferred_command_) {
    return begin_deferred(now_ms);
  }
  if (std::holds_alternative<BeginRetryForTransaction>(exit) && transaction_) {
    const MonotonicMs due = saturating_add(
        last_command_completed_ms_.value_or(now_ms), kCommandRetryMinDelayMs);
    state_ = now_ms >= due ? CoordinatorState::CommandPending
                           : CoordinatorState::RetryDelay;
    context_ = now_ms >= due ? StateContext(CommandPendingContext{due})
                             : StateContext(RetryDelayContext{due});
    return {};
  }
  if (std::holds_alternative<BeginRecoveryQuietWait>(exit)) {
    state_ = CoordinatorState::RecoveryQuietWait;
    context_ = RecoveryContext{RecoveryCause::OemActivity};
    return {};
  }
  if (std::holds_alternative<BeginRecoveryRetryWait>(exit)) {
    state_ = CoordinatorState::RecoveryRetryWait;
    context_ = RecoveryContext{RecoveryCause::OemActivity};
    return {};
  }
  const auto rs = recovery_.snapshot();
  if (rs.cause && *rs.cause == RecoveryCause::EstimatedTimerExpiry &&
      now_ms <= rs.expires_ms) {
    state_ = CoordinatorState::RecoveryQuietWait;
    context_ = RecoveryContext{*rs.cause};
  } else {
    recovery_.cancel();
    state_ = CoordinatorState::Idle;
    context_ = IdleContext{};
  }
  return {};
}

CoreEffects ConfirmationCore::handle_tail_frame(EventKind kind,
                                                MonotonicMs now_ms) {
  if (kind == EventKind::TailExpired)
    return expire_tail(now_ms);
  if (kind == EventKind::LocalTailContradiction)
    authority_.invalidate(AuthorityLossReason::ContradictoryTail, now_ms);
  if (state_ == CoordinatorState::PostCommandListening && transaction_) {
    const auto tx = transaction_->snapshot();
    state_ = CoordinatorState::PostCommandTailWait;
    context_ = PostCommandTailContext{tx.id, AttemptNumber(tx.attempts_started),
                                      epoch_identity_};
  }
  return {};
}
CoreEffects ConfirmationCore::handle_recovery_due(const RecoveryDue &due) {
  if (due.status == RecoveryDueStatus::Expired) {
    recovery_.cancel();
    state_ = CoordinatorState::Idle;
    context_ = IdleContext{};
  } else if (due.status == RecoveryDueStatus::QueryDue) {
    state_ = CoordinatorState::RecoveryQueryPending;
    context_ = QueryPendingContext{QueryPurpose::Recovery, *due.reason};
  }
  return {};
}
CoreEffects ConfirmationCore::handle_timer_expiry(MonotonicMs deadline,
                                                  MonotonicMs now_ms) {
  const auto expiry = authority_.timer_estimate_expired(now_ms);
  CoreEffects effects;
  if (expiry.status != TimerExpiryStatus::Due)
    return effects;
  cancel_passive_observation();
  if (expiry.persistence)
    effects.add(RequestPersistenceEffect{*expiry.persistence});
  if (recovery_.snapshot().cause != RecoveryCause::OemActivity)
    recovery_.arm_from_timer_expiry(deadline);
  if (state_ == CoordinatorState::Idle) {
    state_ = CoordinatorState::RecoveryQuietWait;
    context_ = RecoveryContext{RecoveryCause::EstimatedTimerExpiry};
  }
  return effects;
}
ReceiveContext ConfirmationCore::receive_context() const {
  if (state_ == CoordinatorState::Idle)
    return PassiveObservationContext{};
  if (!window_)
    return NoLocalEpoch{};
  if (state_ == CoordinatorState::ResponseTailQuarantine) {
    const auto &tail = std::get<TailQuarantineContext>(context_);
    return ClassificationTail{*window_, tail.expected_state,
                              tail.epoch_identity};
  }
  if (state_ == CoordinatorState::PostCommandTailWait)
    return ClassificationTail{*window_, safe_tail_state(), epoch_identity_};
  if (state_ == CoordinatorState::PostCommandListening ||
      state_ == CoordinatorState::FallbackResponseListening ||
      state_ == CoordinatorState::BootResponseListening ||
      state_ == CoordinatorState::ManualResponseListening ||
      state_ == CoordinatorState::RecoveryResponseListening) {
    const auto snap = consensus_.snapshot();
    return ActiveResponseWindow{
        *window_, epoch_identity_,
        snap.has_group ? std::optional<FanState>(
                             FanState::observed(snap.latest_raw_byte).value())
                       : std::nullopt};
  }
  return NoLocalEpoch{};
}

} // namespace quietcool
