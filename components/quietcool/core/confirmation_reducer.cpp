#include "confirmation_core.h"

namespace quietcool {

const TransitionRule *
ConfirmationCore::select_rule(const ReducerInput &input) const {
  const auto rules = TransitionTable::rules();
  for (std::size_t index = 0; index < rules.size; ++index) {
    const auto &rule = rules.data[index];
    if (rule.state == state_ && rule.event == input.kind &&
        guard_matches(rule.guard, input))
      return &rule;
  }
  return nullptr;
}
bool ConfirmationCore::guard_matches(GuardId guard,
                                     const ReducerInput &input) const {
  if (guard == GuardId::Always)
    return true;
  if (guard == GuardId::MatchingToken)
    return input.token && token_matches(*input.token);
  if (guard == GuardId::TailOwnsTransaction)
    return transaction_.has_value();
  if (guard == GuardId::TailBeginsRecoveryRetry) {
    const auto *tail = std::get_if<TailQuarantineContext>(&context_);
    return tail && std::holds_alternative<BeginRecoveryRetryWait>(tail->exit);
  }
  if (guard == GuardId::CanLease) {
    if (!sender_ || live_tx_ || tx_tokens_.exhausted())
      return false;
    if (const auto *pending = std::get_if<CommandPendingContext>(&context_))
      return transaction_ && transaction_->may_emit_another_command() &&
             input.now_ms >= pending->earliest_tx_ms;
    return true;
  }
  if (guard == GuardId::CandidateInEpoch) {
    const auto *candidate =
        input.classified ? std::get_if<LocalResponseCandidate>(input.classified)
                         : nullptr;
    return candidate && candidate->epoch_identity == epoch_identity_;
  }
  if (!input.consensus || !transaction_)
    return false;
  const auto tx = transaction_->snapshot();
  const bool prior =
      tx.prior_authority && tx.prior_authority->valid_at_capture &&
      input.consensus->state.semantically_equals(tx.prior_authority->state);
  return transaction_guard_matches(
      guard, {input.consensus->state.semantically_equals(tx.requested),
              tx.requested.is_on(), input.consensus->state.is_on(),
              input.consensus->state.has_outbound_command_marker(),
              prior ? PriorRelation::Equal
                    : (tx.prior_authority ? PriorRelation::Unequal
                                          : PriorRelation::Absent),
              transaction_->may_emit_another_command()});
}
CoreEffects ConfirmationCore::reduce(const ReducerInput &input) {
  const auto *rule = select_rule(input);
  if (!rule) {
    CoreEffects effects;
    const bool tx_event = input.kind == EventKind::TxBurstStarted ||
                          input.kind == EventKind::TxBurstComplete ||
                          input.kind == EventKind::TxBurstRejected ||
                          input.kind == EventKind::TxBurstFault ||
                          input.kind == EventKind::TxLeaseWatchdogFired ||
                          input.kind == EventKind::TxBurstWatchdogFired;
    if (tx_event)
      effects.add(PublishCoreEvent{
          {CoreEventKind::StaleTxCallback, state_, {}, {}, input.token}});
    else if (input.kind == EventKind::ConsensusReached)
      effects.add(PublishCoreEvent{
          {CoreEventKind::InvalidInternalEvent, state_, {}, {}, {}}});
    return effects;
  }
  auto effects = dispatch(rule->action, input);
  const auto fixed = TransitionTable::fixed_next_state(rule->next, rule->state);
  if (fixed)
    state_ = *fixed;
  return effects;
}
CoreEffects ConfirmationCore::dispatch(ActionId action,
                                       const ReducerInput &input) {
  switch (action) {
  case ActionId::HandleRestore:
    return handle_restore(*input.restored, input.now_ms);
  case ActionId::HandleRadioReady:
    return handle_radio_ready(input.now_ms);
  case ActionId::HandleCommandRequest:
    return handle_command_request(*input.requested, input.now_ms);
  case ActionId::AcceptRefresh:
  case ActionId::RefuseRefresh:
    return handle_manual_refresh(action, input.now_ms);
  case ActionId::HandleLearnRequest:
    return handle_learn(*input.learn_mode, input.now_ms);
  case ActionId::HandleForget:
    return handle_forget(input.now_ms);
  case ActionId::HandleLearningFrame:
    return handle_learning_frame(*input.frame, input.now_ms);
  case ActionId::IssueCommandLease:
    return issue_command(input.now_ms);
  case ActionId::IssueQueryLease: {
    const auto *pending = std::get_if<QueryPendingContext>(&context_);
    return pending
               ? issue_query(pending->purpose, pending->reason, input.now_ms)
               : issue_query(QueryPurpose::Fallback,
                             TxReason::TransactionFallbackQuery, input.now_ms);
  }
  case ActionId::StartCommand:
  case ActionId::StartQuery:
    return handle_tx_started(*input.token, input.now_ms);
  case ActionId::CompleteCommand:
  case ActionId::OpenQueryResponse:
    return handle_tx_complete(*input.token, input.now_ms);
  case ActionId::BeginRadioRecovery:
    return handle_radio_fault(input.kind, input.token, input.now_ms);
  case ActionId::TrackCandidate: {
    const auto &candidate = std::get<LocalResponseCandidate>(*input.classified);
    const auto reached = consensus_.observe(candidate.response, input.now_ms);
    if (!reached)
      return {};
    ReducerInput next{EventKind::ConsensusReached, input.now_ms};
    next.consensus = &*reached;
    return reduce(next);
  }
  case ActionId::ApplyConsensus:
  case ActionId::ConfirmAndPromote:
  case ActionId::YieldToOem:
  case ActionId::RetryWithoutPromotion:
  case ActionId::ApplyMismatchWithRetry:
  case ActionId::ExhaustMismatch:
    return apply_consensus(*input.consensus, action, input.now_ms);
  case ActionId::FinishQueryMiss:
  case ActionId::HandlePostWindowMiss:
    return close_window_as_miss(input.now_ms);
  case ActionId::CreateFallback:
    return expire_tail(input.now_ms);
  case ActionId::HandleTailFrame:
    return handle_tail_frame(input.kind, input.now_ms);
  case ActionId::AssertOemPriority:
    return assert_oem_priority(input.now_ms, false);
  case ActionId::HandleExternalState: {
    const auto &value = std::get<ExternalPriorityState>(*input.classified);
    authority_.record_diagnostic(value.state);
    return assert_oem_priority(input.now_ms, true);
  }
  case ActionId::HandleRetryDue:
    context_ = CommandPendingContext{input.now_ms};
    return {};
  case ActionId::HandleHoldoffExpired:
    if (deferred_command_)
      return begin_deferred(input.now_ms);
    state_ = CoordinatorState::RecoveryQuietWait;
    context_ = RecoveryContext{RecoveryCause::OemActivity};
    return {};
  case ActionId::HandleRecoveryDue:
    return handle_recovery_due(*input.recovery_due);
  case ActionId::HandleLearnExpiry:
    learn_.poll(input.now_ms);
    state_ = sender_ ? CoordinatorState::Idle : CoordinatorState::Unprovisioned;
    context_ = sender_ ? StateContext(IdleContext{})
                       : StateContext(UnprovisionedContext{});
    return {};
  case ActionId::HandleTimerExpiry:
    return handle_timer_expiry(*input.timer_deadline, input.now_ms);
  case ActionId::HandleRadioRecovered:
    return handle_radio_recovered(input.now_ms);
  case ActionId::PublishDiagnostic: {
    CoreEffects effects;
    effects.add(
        PublishCoreEvent{{CoreEventKind::Diagnostic, state_, {}, {}, {}}});
    return effects;
  }
  case ActionId::InvalidInternalEvent: {
    CoreEffects effects;
    effects.add(PublishCoreEvent{
        {CoreEventKind::InvalidInternalEvent, state_, {}, {}, {}}});
    return effects;
  }
  case ActionId::None:
  case ActionId::IgnoreLearningOem:
    break;
  }
  return {};
}
CoreEffects ConfirmationCore::restore(const RestorableState &restored,
                                      MonotonicMs now_ms) {
  ReducerInput input{EventKind::ProvisioningRestored, now_ms};
  input.restored = &restored;
  return reduce(input);
}
CoreEffects ConfirmationCore::on_radio_ready(MonotonicMs now_ms) {
  return reduce({EventKind::RadioReady, now_ms});
}
CoreEffects ConfirmationCore::request_state(FanState requested,
                                            MonotonicMs now_ms) {
  ReducerInput input{EventKind::CommandRequested, now_ms};
  input.requested = &requested;
  return reduce(input);
}
CoreEffects ConfirmationCore::request_manual_refresh(MonotonicMs now_ms) {
  return reduce({EventKind::ManualRefreshRequested, now_ms});
}
CoreEffects ConfirmationCore::request_learn(LearnMode mode,
                                            MonotonicMs now_ms) {
  ReducerInput input{EventKind::LearnRequested, now_ms};
  input.learn_mode = &mode;
  return reduce(input);
}
CoreEffects ConfirmationCore::request_forget(MonotonicMs now_ms) {
  return reduce({EventKind::ForgetRequested, now_ms});
}
CoreEffects ConfirmationCore::on_tx_started(TxToken token, MonotonicMs now_ms) {
  ReducerInput input{EventKind::TxBurstStarted, now_ms};
  input.token = token;
  return reduce(input);
}
CoreEffects ConfirmationCore::on_tx_complete(TxToken token,
                                             MonotonicMs now_ms) {
  ReducerInput input{EventKind::TxBurstComplete, now_ms};
  input.token = token;
  return reduce(input);
}
CoreEffects ConfirmationCore::on_tx_rejected(TxToken token,
                                             MonotonicMs now_ms) {
  ReducerInput input{EventKind::TxBurstRejected, now_ms};
  input.token = token;
  return reduce(input);
}
CoreEffects ConfirmationCore::on_tx_fault(TxToken token,
                                          MonotonicMs now_ms) {
  ReducerInput input{EventKind::TxBurstFault, now_ms};
  input.token = token;
  return reduce(input);
}
CoreEffects ConfirmationCore::on_radio_recovered(MonotonicMs now_ms) {
  return reduce({EventKind::RadioRecovered, now_ms});
}

CoreEffects ConfirmationCore::on_frame(ByteView input, MonotonicMs now_ms) {
  if (state_ == CoordinatorState::LearningAwaitingFirst ||
      state_ == CoordinatorState::LearningAwaitingSecond) {
    ReducerInput event{EventKind::FrameReceived, now_ms};
    event.frame = &input;
    return reduce(event);
  }
  if (!sender_)
    return {};
  const auto classified =
      classifier_.classify(input, *sender_, receive_context(), now_ms);
  EventKind kind = EventKind::FrameReceived;
  if (std::holds_alternative<ExactOemQuery>(classified))
    kind = EventKind::ExactOemQueryHeard;
  else if (std::holds_alternative<ExternalPriorityState>(classified))
    kind = EventKind::ExternalStateHeard;
  else if (std::holds_alternative<LocalResponseCandidate>(classified))
    kind = EventKind::ResponseCandidate;
  else if (std::holds_alternative<LocalTailRepeat>(classified))
    kind = EventKind::LocalTailRepeat;
  else if (std::holds_alternative<LocalTailContradiction>(classified))
    kind = EventKind::LocalTailContradiction;
  else if (std::holds_alternative<IgnoredPostCommandPreAcceptanceState>(
               classified))
    kind = EventKind::IgnoredPostCommandPreAcceptanceState;
  else
    return {};
  ReducerInput event{kind, now_ms};
  event.classified = &classified;
  return reduce(event);
}

CoreEffects ConfirmationCore::poll(MonotonicMs now_ms) {
  const auto permission = TransitionTable::rf_permission(state_);
  ReducerInput event{EventKind::Poll, now_ms};
  if (window_ && permission.accept_response) {
    const auto position = window_->position_at(now_ms);
    if (position == WindowPosition::ClassificationTail ||
        position == WindowPosition::Expired)
      event.kind = EventKind::WindowExpired;
  }
  if (window_ &&
      (state_ == CoordinatorState::PostCommandTailWait ||
       state_ == CoordinatorState::ResponseTailQuarantine) &&
      window_->position_at(now_ms) == WindowPosition::Expired)
    event.kind = EventKind::TailExpired;
  if (event.kind == EventKind::Poll && state_ == CoordinatorState::OemHoldoff) {
    const auto age = elapsed_since(
        now_ms, std::get<OemHoldoffContext>(context_).latest_activity_ms);
    if (age && *age >= kOemHoldoffMs)
      event.kind = EventKind::OemHoldoffExpired;
  }
  if (event.kind == EventKind::Poll &&
      (state_ == CoordinatorState::RecoveryQuietWait ||
       state_ == CoordinatorState::RecoveryRetryWait)) {
    const auto due = recovery_.poll(now_ms);
    if (due.status != RecoveryDueStatus::NotDue) {
      event.kind = EventKind::RecoveryDue;
      event.recovery_due = due;
    }
  }
  if (event.kind == EventKind::Poll && state_ == CoordinatorState::RetryDelay &&
      now_ms >= std::get<RetryDelayContext>(context_).due_ms)
    event.kind = EventKind::RetryDue;
  if (event.kind == EventKind::Poll &&
      (state_ == CoordinatorState::LearningAwaitingFirst ||
       state_ == CoordinatorState::LearningAwaitingSecond) &&
      now_ms >= learn_.snapshot().deadline_ms)
    event.kind = EventKind::LearnWindowExpired;
  const auto deadline = authority_.timer_deadline();
  if (event.kind == EventKind::Poll && deadline && now_ms >= *deadline) {
    event.kind = EventKind::TimerEstimateExpired;
    event.timer_deadline = deadline;
  }
  if (event.kind == EventKind::Poll) {
    std::optional<TxToken> token;
    std::optional<MonotonicMs> anchor;
    EventKind watchdog = EventKind::TxLeaseWatchdogFired;
    if (const auto *value = std::get_if<CommandLeaseContext>(&context_)) {
      token = value->token;
      anchor = value->leased_ms;
    } else if (const auto *value = std::get_if<QueryLeaseContext>(&context_)) {
      token = value->token;
      anchor = value->leased_ms;
    } else if (const auto *value = std::get_if<CommandTxContext>(&context_)) {
      token = value->token;
      anchor = value->started_ms;
      watchdog = EventKind::TxBurstWatchdogFired;
    } else if (const auto *value = std::get_if<QueryTxContext>(&context_)) {
      token = value->token;
      anchor = value->started_ms;
      watchdog = EventKind::TxBurstWatchdogFired;
    } else if (const auto *value =
                   std::get_if<RadioRecoveryContext>(&context_)) {
      anchor = value->reset_requested_ms;
      watchdog = EventKind::TxBurstWatchdogFired;
    }
    const auto age = anchor ? elapsed_since(now_ms, *anchor) : std::nullopt;
    if (age && *age >= (watchdog == EventKind::TxLeaseWatchdogFired
                            ? kTxLeaseStartWatchdogMs
                            : kTxBurstWatchdogMs)) {
      event.kind = watchdog;
      event.token = token;
    }
  }
  return reduce(event);
}

bool ConfirmationCore::token_matches(TxToken token) const {
  return live_tx_ && live_tx_->token == token;
}

} // namespace quietcool
