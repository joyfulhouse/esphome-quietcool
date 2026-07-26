#include "confirmation_core.h"

namespace quietcool {

ConfirmationCore::ConfirmationCore(const CoreConfig &config)
    : recovery_(config.jitter_seed) {}
CoreEffects ConfirmationCore::handle_restore(const RestorableState &restored,
                                             MonotonicMs now_ms) {
  CoreEffects effects;
  if (live_tx_)
    effects.add(RevokeTxLease{live_tx_->token});
  transaction_.reset();
  clear_deferred();
  window_.reset();
  consensus_.reset();
  recovery_.cancel();
  learn_.cancel();
  live_tx_.reset();
  last_command_completed_ms_.reset();
  last_outcome_.reset();
  epoch_identity_ = 0;
  command_bursts_ = 0;
  query_bursts_ = 0;
  transaction_ids_.reset();
  tx_tokens_.reset();
  radio_ready_seen_ = false;
  const bool valid = restorable_state_is_valid(restored);
  const RestorableState empty{};
  authority_.restore_hint(valid ? restored : empty, now_ms);
  sender_ = valid ? restored.sender : std::nullopt;
  if (sender_) {
    state_ = CoordinatorState::Idle;
    context_ = IdleContext{};
  } else {
    authority_.invalidate(AuthorityLossReason::Unprovisioned, now_ms);
    state_ = CoordinatorState::Unprovisioned;
    context_ = UnprovisionedContext{};
  }
  // Publish the restored snapshot (issue #31): restore previously produced no
  // PublishAuthorityEffect at all, so the fan entity never saw the restored
  // speed_capability and kept its compiled default of 3 until the boot query's
  // reply. The publication gate swallows this for STATE purposes (authority is
  // Unknown here), but the adapter seeds its supported speed count from it
  // before the gate runs.
  effects.add(PublishAuthorityEffect{authority_.snapshot(now_ms)});
  return effects;
}
CoreEffects ConfirmationCore::handle_radio_ready(MonotonicMs) {
  CoreEffects effects;
  if (radio_ready_seen_)
    return effects;
  radio_ready_seen_ = true;
  if (state_ == CoordinatorState::Idle && sender_) {
    state_ = CoordinatorState::BootQueryPending;
    context_ = QueryPendingContext{QueryPurpose::Boot, TxReason::BootQuery};
  }
  return effects;
}
CoreEffects
ConfirmationCore::begin_transaction(FanState request, MonotonicMs now_ms,
                                    std::optional<PriorAuthoritySnapshot> prior,
                                    bool publish_accept) {
  const auto id = transaction_ids_.allocate();
  if (!id)
    return refuse(RefusalReason::IdExhausted);
  transaction_ = CommandTransaction::begin(*id, request, std::move(prior));
  recovery_.cancel();
  authority_.begin_local_command(transaction_->snapshot().id, now_ms);
  state_ = CoordinatorState::CommandPending;
  context_ = CommandPendingContext{now_ms};
  CoreEffects effects;
  if (publish_accept)
    effects.add(PublishCoreEvent{{CoreEventKind::RequestAccepted, state_,
                                  std::nullopt, std::nullopt, std::nullopt}});
  return effects;
}
void ConfirmationCore::defer_command(
    FanState request, std::optional<PriorAuthoritySnapshot> prior,
    CoreEffects &effects) {
  if (deferred_command_) {
    last_outcome_ = TransactionOutcome::Superseded;
    effects.add(PublishCoreEvent{{CoreEventKind::TransactionFinished,
                                  state_,
                                  {},
                                  TransactionOutcome::Superseded,
                                  {}}});
  }
  deferred_command_ = request;
  deferred_prior_ = std::move(prior);
  effects.add(
      PublishCoreEvent{{CoreEventKind::RequestAccepted, state_, {}, {}, {}}});
}
void ConfirmationCore::clear_deferred() {
  deferred_command_.reset();
  deferred_prior_.reset();
}
CoreEffects ConfirmationCore::begin_deferred(MonotonicMs now_ms) {
  if (!deferred_command_)
    return {};
  const auto request = *deferred_command_;
  const auto prior = deferred_prior_;
  clear_deferred();
  return begin_transaction(request, now_ms, prior, false);
}
CoreEffects ConfirmationCore::handle_command_request(FanState requested,
                                                     MonotonicMs now_ms) {
  if (!sender_)
    return refuse(RefusalReason::Unprovisioned);
  if (!requested.is_valid_command())
    return refuse(RefusalReason::InvalidState);
  if ((transaction_ && transaction_->compare_request(requested) ==
                           JoinDecision::SemanticDuplicate) ||
      (deferred_command_ && deferred_command_->semantically_equals(requested)))
    return {};
  if (transaction_ids_.exhausted())
    return refuse(RefusalReason::IdExhausted);
  const auto prior = transaction_
                         ? transaction_->snapshot().prior_authority
                         : (deferred_prior_ ? deferred_prior_
                                            : authority_.capture_prior(now_ms));
  const auto expected = safe_tail_state();
  const auto original_state = state_;
  CoreEffects effects;
  if (state_ == CoordinatorState::OemHoldoff) {
    defer_command(requested, prior, effects);
    return effects;
  }
  if (state_ == CoordinatorState::ResponseTailQuarantine) {
    if (transaction_)
      finish_transaction(TransactionOutcome::Superseded, effects);
    defer_command(requested, prior, effects);
    auto tail = std::get<TailQuarantineContext>(context_);
    tail.exit = BeginDeferredCommand{};
    context_ = tail;
    return effects;
  }
  if (state_ == CoordinatorState::RadioRecovery) {
    if (transaction_)
      finish_transaction(TransactionOutcome::Superseded, effects);
    defer_command(requested, prior, effects);
    return effects;
  }
  if (state_ == CoordinatorState::CommandTransmitting ||
      std::holds_alternative<QueryTxContext>(context_)) {
    const auto token =
        live_tx_ ? std::optional<TxToken>(live_tx_->token) : std::nullopt;
    auto recovery =
        handle_radio_fault(EventKind::TxBurstWatchdogFired, token, now_ms);
    for (std::size_t i = 0; i < recovery.size(); ++i)
      effects.add(recovery[i]);
    if (transaction_)
      finish_transaction(TransactionOutcome::Superseded, effects);
    defer_command(requested, prior, effects);
    return effects;
  }
  if (const auto *lease = std::get_if<QueryLeaseContext>(&context_)) {
    const auto purpose = lease->purpose;
    const auto anchor = lease->leased_ms;
    if (live_tx_)
      effects.add(RevokeTxLease{live_tx_->token});
    live_tx_.reset();
    if (transaction_)
      finish_transaction(TransactionOutcome::Superseded, effects);
    if (purpose == QueryPurpose::Fallback) {
      ++epoch_identity_;
      window_ = ResponseWindow::classification_tail(anchor);
      consensus_.reset();
      defer_command(requested, prior, effects);
      enter_tail(BeginDeferredCommand{}, expected);
      return effects;
    }
  } else if (live_tx_ && state_ == CoordinatorState::CommandLeaseIssued) {
    effects.add(RevokeTxLease{live_tx_->token});
    live_tx_.reset();
  }
  const bool tail_owned =
      window_ &&
      (original_state == CoordinatorState::PostCommandListening ||
       original_state == CoordinatorState::PostCommandTailWait ||
       original_state == CoordinatorState::FallbackResponseListening ||
       original_state == CoordinatorState::BootResponseListening ||
       original_state == CoordinatorState::ManualResponseListening ||
       original_state == CoordinatorState::RecoveryResponseListening);
  if (transaction_)
    finish_transaction(TransactionOutcome::Superseded, effects);
  if (tail_owned) {
    defer_command(requested, prior, effects);
    enter_tail(BeginDeferredCommand{}, expected);
    return effects;
  }
  if (original_state == CoordinatorState::LearningAwaitingFirst ||
      original_state == CoordinatorState::LearningAwaitingSecond)
    learn_.cancel();
  MonotonicMs earliest = now_ms;
  if (const auto *pending = std::get_if<CommandPendingContext>(&context_))
    earliest = pending->earliest_tx_ms;
  if (const auto *delay = std::get_if<RetryDelayContext>(&context_))
    earliest = delay->due_ms;
  auto started = begin_transaction(requested, now_ms, prior);
  for (std::size_t i = 0; i < started.size(); ++i)
    effects.add(started[i]);
  if (transaction_ && now_ms < earliest) {
    state_ = CoordinatorState::RetryDelay;
    context_ = RetryDelayContext{earliest};
  }
  return effects;
}
CoreEffects ConfirmationCore::handle_manual_refresh(ActionId action,
                                                    MonotonicMs now_ms) {
  if (action == ActionId::RefuseRefresh) {
    if (!sender_)
      return refuse(RefusalReason::Unprovisioned);
    if (state_ == CoordinatorState::OemHoldoff)
      return refuse(RefusalReason::Holdoff);
    if (state_ == CoordinatorState::LearningAwaitingFirst ||
        state_ == CoordinatorState::LearningAwaitingSecond)
      return refuse(RefusalReason::Learning);
    return refuse(RefusalReason::Busy);
  }
  authority_.begin_manual_revalidation(now_ms);
  recovery_.cancel();
  state_ = CoordinatorState::ManualQueryPending;
  context_ = QueryPendingContext{QueryPurpose::Manual, TxReason::ManualQuery};
  return {};
}
CoreEffects ConfirmationCore::handle_learn(LearnMode mode, MonotonicMs now_ms) {
  CoreEffects effects;
  if (live_tx_) {
    effects.add(RevokeTxLease{live_tx_->token});
    live_tx_.reset();
  }
  if (transaction_)
    finish_transaction(TransactionOutcome::CancelledForLearning, effects);
  clear_deferred();
  recovery_.cancel();
  window_.reset();
  consensus_.reset();
  authority_.invalidate(AuthorityLossReason::LearningStarted, now_ms);
  learn_.start(mode, now_ms);
  state_ = CoordinatorState::LearningAwaitingFirst;
  context_ = LearningContext{mode, learn_.snapshot().deadline_ms};
  return effects;
}
CoreEffects ConfirmationCore::handle_forget(MonotonicMs now_ms) {
  CoreEffects effects;
  if (live_tx_)
    effects.add(RevokeTxLease{live_tx_->token});
  live_tx_.reset();
  transaction_.reset();
  clear_deferred();
  sender_.reset();
  window_.reset();
  consensus_.reset();
  recovery_.cancel();
  learn_.cancel();
  // The binding is gone, so the fan-bound sticky capability goes with it —
  // in RAM as well as in NVS (EraseProvisioning below). Forget may precede
  // learning a DIFFERENT fan, and the old fan's capability must not re-aim
  // that fan's commands in the meantime (issue #31).
  authority_.clear_confirmed_capability();
  authority_.invalidate(AuthorityLossReason::Unprovisioned, now_ms);
  state_ = CoordinatorState::Unprovisioned;
  context_ = UnprovisionedContext{};
  effects.add(RequestPersistenceEffect{{PersistenceKind::EraseProvisioning,
                                        std::nullopt, std::nullopt,
                                        std::nullopt}});
  return effects;
}

CoreEffects ConfirmationCore::handle_learning_frame(ByteView input,
                                                    MonotonicMs now_ms) {
  CoreEffects effects;
  const auto event = learn_.observe(input, now_ms);
  if (event.kind == LearnEventKind::CandidateStarted ||
      event.kind == LearnEventKind::CandidateRestarted) {
    state_ = CoordinatorState::LearningAwaitingSecond;
    context_ =
        LearningContext{learn_.snapshot().mode, learn_.snapshot().deadline_ms};
  } else if (event.kind == LearnEventKind::Learned) {
    // Re-binding to a DIFFERENT fan discards the sticky capability: it
    // described the previous fan, and left in place it would re-aim the new
    // fan's commands (and get_traits) against the wrong band until the first
    // confirmed report (issue #31). Re-learning the SAME fan keeps it — the
    // fan did not change. The SaveProvisioning below carries the surviving
    // value so NVS is re-bound atomically with the sender: the adapter must
    // not re-persist the old fan's capability under the new sender.
    if (!(sender_ == event.sender)) authority_.clear_confirmed_capability();
    sender_ = event.sender;
    recovery_.cancel();
    authority_.invalidate(AuthorityLossReason::SenderChanged, now_ms);
    state_ = CoordinatorState::Idle;
    context_ = IdleContext{};
    effects.add(RequestPersistenceEffect{
        {PersistenceKind::SaveProvisioning, sender_, std::nullopt,
         authority_.snapshot(now_ms).speed_capability}});
  } else if (event.kind == LearnEventKind::AmbiguousRejected) {
    // Two distinct fans were heard in one window; refuse and keep whatever was
    // bound before. sender_ is left untouched, so no SaveProvisioning is
    // emitted and the previously bound fan survives. Mirrors the expiry path
    // (HandleLearnExpiry); both learning-frame rules are NextStateId::Computed,
    // so this in-handler state assignment is authoritative.
    learn_.cancel();
    state_ = sender_ ? CoordinatorState::Idle : CoordinatorState::Unprovisioned;
    context_ = sender_ ? StateContext(IdleContext{})
                       : StateContext(UnprovisionedContext{});
    effects.add(RefusedInput{RefusalReason::AmbiguousLearn, state_});
  }
  return effects;
}

void ConfirmationCore::finish_transaction(TransactionOutcome outcome,
                                          CoreEffects &effects) {
  if (!transaction_)
    return;
  transaction_->finish(outcome);
  last_outcome_ = outcome;
  effects.add(PublishCoreEvent{
      {CoreEventKind::TransactionFinished, state_, {}, outcome, {}}});
  transaction_.reset();
}
void ConfirmationCore::enter_tail(TailExit exit, FanState expected) {
  state_ = CoordinatorState::ResponseTailQuarantine;
  context_ = TailQuarantineContext{std::move(exit), epoch_identity_, expected};
}

CoreEffects ConfirmationCore::refuse(RefusalReason reason) const {
  CoreEffects effects;
  effects.add(RefusedInput{reason, state_});
  return effects;
}
CoreSnapshot ConfirmationCore::snapshot(MonotonicMs now_ms) const {
  return {state_,
          context_,
          authority_.snapshot(now_ms),
          transaction_
              ? std::optional<TransactionSnapshot>(transaction_->snapshot())
              : std::nullopt,
          recovery_.snapshot(),
          learn_.snapshot(),
          live_tx_,
          last_outcome_,
          deferred_command_,
          epoch_identity_,
          command_bursts_,
          query_bursts_};
}

} // namespace quietcool
