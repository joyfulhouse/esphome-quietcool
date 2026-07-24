#include "confirmation_core.h"

namespace quietcool {

CoreEffects ConfirmationCore::issue_command(MonotonicMs now_ms) {
  if (!sender_ || !transaction_ || !transaction_->may_emit_another_command())
    return {};
  const auto tx = transaction_->snapshot();
  const auto payload = FrameCodec::encode_state(*sender_, tx.outbound_command);
  if (!payload)
    return refuse(RefusalReason::InvalidState);
  const auto token = tx_tokens_.allocate();
  if (!token)
    return refuse(RefusalReason::IdExhausted);
  live_tx_ = TxRequest{
      *token,
      payload.value(),
      TxReason::TransactionCommand,
      tx.id,
      AttemptNumber(static_cast<std::uint64_t>(tx.attempts_started) + 1U),
      now_ms};
  state_ = CoordinatorState::CommandLeaseIssued;
  context_ = CommandLeaseContext{live_tx_->token, now_ms};
  CoreEffects effects;
  effects.add(RequestTxBurst{*live_tx_});
  return effects;
}
CoreEffects ConfirmationCore::issue_query(QueryPurpose purpose, TxReason reason,
                                          MonotonicMs now_ms) {
  if (!sender_)
    return {};
  const auto transaction =
      transaction_
          ? std::optional<TransactionSnapshot>(transaction_->snapshot())
          : std::nullopt;
  if (purpose == QueryPurpose::Fallback && !transaction)
    return {};
  if (purpose == QueryPurpose::Fallback &&
      !transaction->fallback_used_for_attempt &&
      !transaction_->mark_fallback_used())
    return {};
  const auto token = tx_tokens_.allocate();
  if (!token)
    return refuse(RefusalReason::IdExhausted);
  const auto tx = transaction
                      ? std::optional<TransactionId>(transaction->id)
                      : std::nullopt;
  const auto attempt =
      transaction ? std::optional<AttemptNumber>(
                        AttemptNumber(transaction->attempts_started))
                  : std::nullopt;
  live_tx_ = TxRequest{*token,
                       FrameCodec::encode_query(*sender_),
                       reason,
                       tx,
                       attempt,
                       now_ms};
  state_ = TransitionTable::query_lease_state(purpose);
  context_ = QueryLeaseContext{purpose, reason, live_tx_->token, now_ms};
  CoreEffects effects;
  effects.add(RequestTxBurst{*live_tx_});
  return effects;
}
CoreEffects ConfirmationCore::handle_tx_started(TxToken token,
                                                MonotonicMs now_ms) {
  CoreEffects effects;
  if (state_ == CoordinatorState::CommandLeaseIssued && transaction_) {
    const auto lease = std::get<CommandLeaseContext>(context_);
    const auto attempt = transaction_->note_command_burst_started();
    if (!attempt)
      return effects;
    live_tx_->attempt = attempt.value();
    ++command_bursts_;
    state_ = CoordinatorState::CommandTransmitting;
    context_ = CommandTxContext{transaction_->snapshot().id, attempt.value(),
                                token, lease.leased_ms, now_ms};
    return effects;
  }
  if (const auto *lease = std::get_if<QueryLeaseContext>(&context_)) {
    ++query_bursts_;
    if (lease->purpose == QueryPurpose::Recovery)
      recovery_.note_query_started(now_ms);
    if (lease->purpose == QueryPurpose::Manual)
      authority_.bind_manual_query_token(token);
    state_ = TransitionTable::query_transmitting_state(lease->purpose);
    context_ = QueryTxContext{lease->purpose, lease->reason, token, now_ms};
    return effects;
  }
  effects.add(PublishCoreEvent{
      {CoreEventKind::StaleTxCallback, state_, {}, {}, token}});
  return effects;
}
CoreEffects ConfirmationCore::handle_tx_complete(TxToken token,
                                                 MonotonicMs now_ms) {
  CoreEffects effects;
  if (state_ == CoordinatorState::CommandTransmitting && transaction_) {
    const auto tx = transaction_->snapshot();
    live_tx_.reset();
    last_command_completed_ms_ = now_ms;
    ++epoch_identity_;
    window_ = ResponseWindow::post_command(
        tx.id, AttemptNumber(tx.attempts_started), now_ms);
    consensus_.reset();
    state_ = CoordinatorState::PostCommandListening;
    context_ = PostCommandContext{tx.id, AttemptNumber(tx.attempts_started),
                                  now_ms, epoch_identity_};
    return effects;
  }
  if (const auto *tx = std::get_if<QueryTxContext>(&context_)) {
    const QueryPurpose purpose = tx->purpose;
    const TxReason reason = tx->reason;
    live_tx_.reset();
    ++epoch_identity_;
    window_ = ResponseWindow::query(purpose, token, tx->started_ms);
    consensus_.reset();
    state_ = TransitionTable::query_response_state(purpose);
    context_ = QueryResponseContext{purpose, reason, token, epoch_identity_};
    return effects;
  }
  effects.add(PublishCoreEvent{
      {CoreEventKind::StaleTxCallback, state_, {}, {}, token}});
  return effects;
}
CoreEffects ConfirmationCore::handle_radio_fault(EventKind kind,
                                                 std::optional<TxToken> token,
                                                 MonotonicMs now_ms) {
  CoreEffects effects;
  if (state_ == CoordinatorState::RadioRecovery) {
    auto recovery = std::get<RadioRecoveryContext>(context_);
    if (recovery.attempts < kRadioRecoveryAttempts) {
      ++recovery.attempts;
      recovery.reset_requested_ms = now_ms;
      context_ = recovery;
      effects.add(RequestRadioReset{recovery.attempts});
      return effects;
    }
    if (transaction_)
      finish_transaction(TransactionOutcome::RadioUnavailable, effects);
    authority_.invalidate(AuthorityLossReason::RadioUnavailable, now_ms);
    recovery_.cancel();
    live_tx_.reset();
    state_ = sender_ ? CoordinatorState::Idle : CoordinatorState::Unprovisioned;
    context_ = sender_ ? StateContext(IdleContext{})
                       : StateContext(UnprovisionedContext{});
    effects.add(
        PublishCoreEvent{{CoreEventKind::WatchdogFired, state_, {}, {}, {}}});
    effects.add(PublishAuthorityEffect{authority_.snapshot(now_ms)});
    return effects;
  }
  if (!live_tx_ || !token)
    return effects;
  const auto reason = live_tx_->reason;
  const auto expected = safe_tail_state();
  RadioRecoveryTarget target = RadioRecoveryTarget::ReturnIdleUnknown;
  MonotonicMs anchor = now_ms;
  if (const auto *lease = std::get_if<CommandLeaseContext>(&context_)) {
    target = RadioRecoveryTarget::ReissueUnstartedCommandLease;
    anchor = lease->leased_ms;
  } else if (const auto *tx = std::get_if<CommandTxContext>(&context_)) {
    target = RadioRecoveryTarget::QuarantineStartedCommandThenRetry;
    anchor = tx->started_ms;
  } else if (const auto *lease = std::get_if<QueryLeaseContext>(&context_)) {
    target = (kind == EventKind::TxBurstRejected &&
              lease->purpose == QueryPurpose::Boot)
                 ? RadioRecoveryTarget::ReturnIdleUnknown
                 : RadioRecoveryTarget::ReissueUnstartedQueryLease;
    anchor = lease->leased_ms;
  } else if (const auto *tx = std::get_if<QueryTxContext>(&context_)) {
    target = RadioRecoveryTarget::FinishStartedQueryAsMiss;
    anchor = tx->started_ms;
  }
  if (kind == EventKind::TxLeaseWatchdogFired)
    effects.add(RevokeTxLease{*token});
  live_tx_.reset();
  state_ = CoordinatorState::RadioRecovery;
  context_ =
      RadioRecoveryContext{target, 1, reason, token, expected, anchor, now_ms};
  effects.add(RequestRadioReset{1});
  return effects;
}
CoreEffects ConfirmationCore::handle_radio_recovered(MonotonicMs now_ms) {
  if (state_ != CoordinatorState::RadioRecovery)
    return {};
  const auto value = std::get<RadioRecoveryContext>(context_);
  if (deferred_command_ &&
      (value.target == RadioRecoveryTarget::ReissueUnstartedCommandLease ||
       value.target == RadioRecoveryTarget::ReissueUnstartedQueryLease ||
       value.target == RadioRecoveryTarget::ReturnIdleUnknown))
    return begin_deferred(now_ms);
  if (value.target == RadioRecoveryTarget::ReissueUnstartedCommandLease &&
      transaction_) {
    state_ = CoordinatorState::CommandPending;
    context_ = CommandPendingContext{now_ms};
    return {};
  }
  if (value.target == RadioRecoveryTarget::ReissueUnstartedQueryLease &&
      value.reason) {
    const auto purpose = TransitionTable::query_purpose(*value.reason);
    const auto recovery = recovery_.snapshot();
    if (*value.reason == TxReason::TimerExpiryRecoveryQuery &&
        now_ms > recovery.expires_ms) {
      recovery_.cancel();
      state_ = CoordinatorState::Idle;
      context_ = IdleContext{};
      return {};
    }
    state_ = TransitionTable::query_pending_state(purpose);
    context_ =
        purpose == QueryPurpose::Fallback && transaction_
            ? StateContext(FallbackQueryContext{
                  transaction_->snapshot().id,
                  AttemptNumber(transaction_->snapshot().attempts_started)})
            : StateContext(QueryPendingContext{purpose, *value.reason});
    return {};
  }
  if (value.target == RadioRecoveryTarget::QuarantineStartedCommandThenRetry) {
    ++epoch_identity_;
    consensus_.reset();
    if (transaction_) {
      const auto tx = transaction_->snapshot();
      last_command_completed_ms_ = value.target_anchor_ms;
      window_ = ResponseWindow::post_command(
          tx.id, AttemptNumber(tx.attempts_started), value.target_anchor_ms);
      enter_tail(BeginRetryForTransaction{tx.id}, tx.requested);
    } else {
      window_ = ResponseWindow::classification_tail(value.target_anchor_ms);
      enter_tail(BeginDeferredCommand{},
                 value.expected_state.value_or(
                     FanState::command(Speed::Low, Duration::Off)));
    }
    return {};
  }
  if (value.target == RadioRecoveryTarget::FinishStartedQueryAsMiss &&
      value.reason && value.token) {
    const auto purpose = TransitionTable::query_purpose(*value.reason);
    ++epoch_identity_;
    window_ =
        ResponseWindow::query(purpose, *value.token, value.target_anchor_ms);
    consensus_.reset();
    if (deferred_command_) {
      enter_tail(BeginDeferredCommand{},
                 value.expected_state.value_or(
                     FanState::command(Speed::Low, Duration::Off)));
      return {};
    }
    state_ = TransitionTable::query_response_state(purpose);
    context_ = QueryResponseContext{purpose, *value.reason, *value.token,
                                    epoch_identity_};
    return close_window_as_miss(now_ms);
  }
  authority_.invalidate(AuthorityLossReason::RadioUnavailable, now_ms);
  state_ = sender_ ? CoordinatorState::Idle : CoordinatorState::Unprovisioned;
  context_ = sender_ ? StateContext(IdleContext{})
                     : StateContext(UnprovisionedContext{});
  return {};
}

} // namespace quietcool
