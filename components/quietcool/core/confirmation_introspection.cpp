#include "confirmation_core.h"

namespace quietcool {

StateContext ConfirmationCore::context_for_test(CoordinatorState state) {
  const TransactionId id(1);
  const AttemptNumber attempt(1);
  const TxToken token(1);
  const auto fan = FanState::command(Speed::Low, Duration::Off);
  switch (state) {
  case CoordinatorState::Unprovisioned:
    return UnprovisionedContext{};
  case CoordinatorState::Idle:
    return IdleContext{};
  case CoordinatorState::CommandPending:
    return CommandPendingContext{0};
  case CoordinatorState::CommandLeaseIssued:
    return CommandLeaseContext{token, 0};
  case CoordinatorState::CommandTransmitting:
    return CommandTxContext{id, attempt, token, 0, 0};
  case CoordinatorState::PostCommandListening:
    return PostCommandContext{id, attempt, 0, 1};
  case CoordinatorState::PostCommandTailWait:
    return PostCommandTailContext{id, attempt, 1};
  case CoordinatorState::FallbackQueryPending:
    return FallbackQueryContext{id, attempt};
  case CoordinatorState::RetryDelay:
    return RetryDelayContext{0};
  case CoordinatorState::OemHoldoff:
    return OemHoldoffContext{0};
  case CoordinatorState::RecoveryQuietWait:
  case CoordinatorState::RecoveryRetryWait:
    return RecoveryContext{RecoveryCause::OemActivity};
  case CoordinatorState::ResponseTailQuarantine:
    return TailQuarantineContext{ReturnIdle{}, 1, fan};
  case CoordinatorState::LearningAwaitingFirst:
  case CoordinatorState::LearningAwaitingSecond:
    return LearningContext{LearnMode::Manual, 0};
  case CoordinatorState::RadioRecovery:
    return RadioRecoveryContext{RadioRecoveryTarget::ReturnIdleUnknown,
                                0,
                                {},
                                token,
                                std::nullopt,
                                0,
                                0};
  // Enumerated rather than defaulted: with a `default` label, a newly added
  // state fell through to the query-family fallback below and was silently
  // mapped to QueryPendingContext, so context_matches_state() would validate
  // the wrong variant instead of failing. Listing them means -Wswitch
  // -Werror rejects the build until the new state is classified here.
  case CoordinatorState::BootQueryPending:
  case CoordinatorState::BootQueryLeaseIssued:
  case CoordinatorState::BootQueryTransmitting:
  case CoordinatorState::BootResponseListening:
  case CoordinatorState::ManualQueryPending:
  case CoordinatorState::ManualQueryLeaseIssued:
  case CoordinatorState::ManualQueryTransmitting:
  case CoordinatorState::ManualResponseListening:
  case CoordinatorState::FallbackQueryLeaseIssued:
  case CoordinatorState::FallbackQueryTransmitting:
  case CoordinatorState::FallbackResponseListening:
  case CoordinatorState::RecoveryQueryPending:
  case CoordinatorState::RecoveryQueryLeaseIssued:
  case CoordinatorState::RecoveryQueryTransmitting:
  case CoordinatorState::RecoveryResponseListening:
    break;
  }
  const bool lease = state == CoordinatorState::BootQueryLeaseIssued ||
                     state == CoordinatorState::ManualQueryLeaseIssued ||
                     state == CoordinatorState::FallbackQueryLeaseIssued ||
                     state == CoordinatorState::RecoveryQueryLeaseIssued;
  const bool tx = state == CoordinatorState::BootQueryTransmitting ||
                  state == CoordinatorState::ManualQueryTransmitting ||
                  state == CoordinatorState::FallbackQueryTransmitting ||
                  state == CoordinatorState::RecoveryQueryTransmitting;
  const bool response = state == CoordinatorState::BootResponseListening ||
                        state == CoordinatorState::ManualResponseListening ||
                        state == CoordinatorState::FallbackResponseListening ||
                        state == CoordinatorState::RecoveryResponseListening;
  const QueryPurpose purpose =
      (state == CoordinatorState::ManualQueryPending ||
       state == CoordinatorState::ManualQueryLeaseIssued ||
       state == CoordinatorState::ManualQueryTransmitting ||
       state == CoordinatorState::ManualResponseListening)
          ? QueryPurpose::Manual
      : (state == CoordinatorState::FallbackQueryLeaseIssued ||
         state == CoordinatorState::FallbackQueryTransmitting ||
         state == CoordinatorState::FallbackResponseListening)
          ? QueryPurpose::Fallback
      : (state == CoordinatorState::RecoveryQueryPending ||
         state == CoordinatorState::RecoveryQueryLeaseIssued ||
         state == CoordinatorState::RecoveryQueryTransmitting ||
         state == CoordinatorState::RecoveryResponseListening)
          ? QueryPurpose::Recovery
          : QueryPurpose::Boot;
  if (lease)
    return QueryLeaseContext{purpose, TxReason::BootQuery, token, 0};
  if (tx)
    return QueryTxContext{purpose, TxReason::BootQuery, token, 0};
  if (response)
    return QueryResponseContext{purpose, TxReason::BootQuery, token, 1};
  return QueryPendingContext{purpose, TxReason::BootQuery};
}
bool ConfirmationCore::context_matches_state(CoordinatorState state,
                                             const StateContext &context) {
  return context.index() == context_for_test(state).index();
}
TransactionRuleMatches ConfirmationCore::matching_transaction_rules_for_test(
    CoordinatorState state, const TransactionConsensusInput &input) {
  TransitionRule first{};
  std::size_t count = 0;
  const auto rules = TransitionTable::rules();
  for (std::size_t index = 0; index < rules.size; ++index) {
    const auto &rule = rules.data[index];
    if (rule.state != state ||
        rule.origin != TemplateOrigin::TransactionConsensus ||
        !transaction_guard_matches(rule.guard, input))
      continue;
    if (count++ == 0)
      first = rule;
  }
  return {first, count};
}
bool ConfirmationCore::action_is_dispatchable_for_test(ActionId action) {
  switch (action) {
  case ActionId::None:
  case ActionId::IssueQueryLease:
  case ActionId::StartQuery:
  case ActionId::BeginRadioRecovery:
  case ActionId::OpenQueryResponse:
  case ActionId::TrackCandidate:
  case ActionId::ApplyConsensus:
  case ActionId::FinishQueryMiss:
  case ActionId::AssertOemPriority:
  case ActionId::IgnoreLearningOem:
  case ActionId::AcceptRefresh:
  case ActionId::RefuseRefresh:
  case ActionId::ConfirmAndPromote:
  case ActionId::YieldToOem:
  case ActionId::RetryWithoutPromotion:
  case ActionId::ApplyMismatchWithRetry:
  case ActionId::ExhaustMismatch:
  case ActionId::CreateFallback:
  case ActionId::HandleRestore:
  case ActionId::HandleRadioReady:
  case ActionId::HandleCommandRequest:
  case ActionId::HandleLearnRequest:
  case ActionId::HandleForget:
  case ActionId::HandleExternalState:
  case ActionId::HandleTimerExpiry:
  case ActionId::IssueCommandLease:
  case ActionId::StartCommand:
  case ActionId::CompleteCommand:
  case ActionId::HandlePostWindowMiss:
  case ActionId::HandleTailFrame:
  case ActionId::HandleRetryDue:
  case ActionId::HandleHoldoffExpired:
  case ActionId::HandleRecoveryDue:
  case ActionId::HandleLearningFrame:
  case ActionId::HandleLearnExpiry:
  case ActionId::HandleRadioRecovered:
  case ActionId::PublishDiagnostic:
  case ActionId::InvalidInternalEvent:
    return true;
  }
  return false;
}

} // namespace quietcool
