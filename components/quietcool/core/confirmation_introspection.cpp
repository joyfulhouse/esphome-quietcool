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
  // Each family carries its OWN canonical reason, not BootQuery. The reason is
  // finer-grained than the purpose (the Recovery family spans three reasons);
  // RecoveryQueryInitial is the natural initial one, and context_matches_state()
  // accepts any reason in the right family, so the other two still validate.
  const TxReason reason = purpose == QueryPurpose::Manual ? TxReason::ManualQuery
      : purpose == QueryPurpose::Fallback ? TxReason::TransactionFallbackQuery
      : purpose == QueryPurpose::Recovery ? TxReason::RecoveryQueryInitial
                                          : TxReason::BootQuery;
  if (lease)
    return QueryLeaseContext{purpose, reason, token, 0};
  if (tx)
    return QueryTxContext{purpose, reason, token, 0};
  if (response)
    return QueryResponseContext{purpose, reason, token, 1};
  return QueryPendingContext{purpose, reason};
}
// The query-family variants (QueryPending/Lease/Tx/Response) each back four
// distinct states (Boot/Manual/Fallback/Recovery), told apart only by this
// interior `purpose`. Comparing variant index alone would accept a state paired
// with a sibling context of the wrong purpose — a genuine desync class, since
// the real dispatcher reads `purpose` back to pick the next transition.
static std::optional<QueryPurpose> query_purpose_of(const StateContext &context) {
  if (const auto *p = std::get_if<QueryPendingContext>(&context)) return p->purpose;
  if (const auto *p = std::get_if<QueryLeaseContext>(&context)) return p->purpose;
  if (const auto *p = std::get_if<QueryTxContext>(&context)) return p->purpose;
  if (const auto *p = std::get_if<QueryResponseContext>(&context)) return p->purpose;
  return std::nullopt;
}
static std::optional<TxReason> query_reason_of(const StateContext &context) {
  if (const auto *p = std::get_if<QueryPendingContext>(&context)) return p->reason;
  if (const auto *p = std::get_if<QueryLeaseContext>(&context)) return p->reason;
  if (const auto *p = std::get_if<QueryTxContext>(&context)) return p->reason;
  if (const auto *p = std::get_if<QueryResponseContext>(&context)) return p->reason;
  return std::nullopt;
}
bool ConfirmationCore::context_matches_state(CoordinatorState state,
                                             const StateContext &context) {
  const auto canonical = context_for_test(state);
  if (context.index() != canonical.index()) return false;
  // For non-query variants both sides are nullopt (equal): behaviour unchanged.
  const auto actual_purpose = query_purpose_of(context);
  const auto canonical_purpose = query_purpose_of(canonical);
  const bool heartbeat_in_manual_family =
      canonical_purpose == QueryPurpose::Manual &&
      actual_purpose == QueryPurpose::Heartbeat;
  if (actual_purpose != canonical_purpose && !heartbeat_in_manual_family)
    return false;
  // TxReason is semantic state (it steers radio-recovery routing), so a context
  // carrying the wrong query family's reason — the class this fixture used to
  // build with BootQuery — must be rejected even when its purpose field is
  // right. TxReason is finer-grained than QueryPurpose: the Recovery family
  // spans RecoveryQueryInitial / RecoveryQueryRetry / TimerExpiryRecoveryQuery.
  // Classify via query_family_of(), NOT the routing query_purpose(): the latter
  // has a catch-all default that folds every non-query reason (e.g.
  // TransactionCommand) into Recovery, which would certify RecoveryQueryPending +
  // TransactionCommand as coherent (#26). query_family_of() yields nullopt for a
  // non-query reason, so it is rejected, while the three Recovery query reasons
  // still map to Recovery — family semantics preserved, exact-reason rejection
  // avoided. Non-query variants carry no reason (both nullopt): unchanged.
  const auto reason = query_reason_of(context);
  if (reason && actual_purpose &&
      TransitionTable::query_family_of(*reason) != actual_purpose)
    return false;
  return true;
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
} // namespace quietcool
