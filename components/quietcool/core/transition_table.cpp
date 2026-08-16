#include "transition_table.h"

#include <array>

namespace quietcool {
namespace {

struct QueryStates final {
  CoordinatorState pending;
  CoordinatorState lease;
  CoordinatorState transmitting;
  CoordinatorState response;
  NextStateId lease_next;
  NextStateId transmitting_next;
  NextStateId response_next;
};

constexpr std::array<QueryStates, 4> kQueryStates{{
    {CoordinatorState::BootQueryPending, CoordinatorState::BootQueryLeaseIssued,
     CoordinatorState::BootQueryTransmitting, CoordinatorState::BootResponseListening,
     NextStateId::BootQueryLeaseIssued, NextStateId::BootQueryTransmitting,
     NextStateId::BootResponseListening},
    {CoordinatorState::ManualQueryPending, CoordinatorState::ManualQueryLeaseIssued,
     CoordinatorState::ManualQueryTransmitting, CoordinatorState::ManualResponseListening,
     NextStateId::ManualQueryLeaseIssued, NextStateId::ManualQueryTransmitting,
     NextStateId::ManualResponseListening},
    {CoordinatorState::FallbackQueryPending, CoordinatorState::FallbackQueryLeaseIssued,
     CoordinatorState::FallbackQueryTransmitting, CoordinatorState::FallbackResponseListening,
     NextStateId::FallbackQueryLeaseIssued, NextStateId::FallbackQueryTransmitting,
     NextStateId::FallbackResponseListening},
    {CoordinatorState::RecoveryQueryPending, CoordinatorState::RecoveryQueryLeaseIssued,
     CoordinatorState::RecoveryQueryTransmitting, CoordinatorState::RecoveryResponseListening,
     NextStateId::RecoveryQueryLeaseIssued, NextStateId::RecoveryQueryTransmitting,
     NextStateId::RecoveryResponseListening}}};
constexpr std::array<TemplateOrigin, 4> kOrigins{
    TemplateOrigin::BootQuery, TemplateOrigin::ManualQuery,
    TemplateOrigin::FallbackQuery, TemplateOrigin::RecoveryQuery};
constexpr std::size_t kRuleCount = 359 + 2U * kCoordinatorStateCount;

constexpr std::size_t query_family_index(QueryPurpose purpose) {
  return static_cast<std::size_t>(
      purpose == QueryPurpose::Heartbeat ? QueryPurpose::Manual : purpose);
}

constexpr NextStateId tail_or_computed(std::size_t family) {
  // Recovery misses consult the retry schedule; other query misses have a fixed tail.
  return family == 3 ? NextStateId::Computed
                     : NextStateId::ResponseTailQuarantine;
}

constexpr void add_rule(std::array<TransitionRule, kRuleCount>& rules,
                        std::size_t& index, CoordinatorState state,
                        EventKind event, GuardId guard, ActionId action,
                        NextStateId next, std::uint16_t priority,
                        TemplateOrigin origin) {
  rules[index] = {state, event, guard, action, next, priority,
                  static_cast<std::uint16_t>(index + 1), origin};
  ++index;
}

constexpr void add_query_family(std::array<TransitionRule, kRuleCount>& rules,
                                std::size_t& index, std::size_t family) {
  const auto s = kQueryStates[family];
  const auto origin = kOrigins[family];
  add_rule(rules, index, s.pending, EventKind::Poll, GuardId::CanLease,
           ActionId::IssueQueryLease, s.lease_next, 1, origin);
  add_rule(rules, index, s.lease, EventKind::TxBurstStarted, GuardId::MatchingToken,
           ActionId::StartQuery, s.transmitting_next, 1, origin);
  add_rule(rules, index, s.lease, EventKind::TxBurstRejected, GuardId::MatchingToken,
           ActionId::BeginRadioRecovery, NextStateId::RadioRecovery, 1, origin);
  add_rule(rules, index, s.lease, EventKind::TxLeaseWatchdogFired, GuardId::MatchingToken,
           ActionId::BeginRadioRecovery, NextStateId::RadioRecovery, 1, origin);
  add_rule(rules, index, s.transmitting, EventKind::TxBurstComplete,
           GuardId::MatchingToken, ActionId::OpenQueryResponse, s.response_next, 1, origin);
  add_rule(rules, index, s.transmitting, EventKind::TxBurstWatchdogFired,
           GuardId::MatchingToken, ActionId::BeginRadioRecovery,
           NextStateId::RadioRecovery, 1, origin);
  add_rule(rules, index, s.transmitting, EventKind::TxBurstFault,
           GuardId::MatchingToken, ActionId::BeginRadioRecovery,
           NextStateId::RadioRecovery, 1, origin);
  add_rule(rules, index, s.response, EventKind::ResponseCandidate,
           GuardId::CandidateInEpoch, ActionId::TrackCandidate,
           NextStateId::Computed, 1, origin);
  add_rule(rules, index, s.response, EventKind::WindowExpired, GuardId::Always,
           ActionId::FinishQueryMiss, tail_or_computed(family), 1, origin);
  add_rule(rules, index, s.response, EventKind::ExternalStateHeard, GuardId::Always,
           ActionId::AssertOemPriority, NextStateId::OemHoldoff, 1, origin);
}

constexpr void add_consensus(std::array<TransitionRule, kRuleCount>& rules,
                             std::size_t& index, CoordinatorState state) {
  add_rule(rules, index, state, EventKind::ConsensusReached, GuardId::SemanticMatch,
           ActionId::ConfirmAndPromote, NextStateId::ResponseTailQuarantine, 1,
           TemplateOrigin::TransactionConsensus);
  add_rule(rules, index, state, EventKind::ConsensusReached,
           GuardId::OnCommandMismatchNotPrior, ActionId::YieldToOem,
           NextStateId::ResponseTailQuarantine, 2, TemplateOrigin::TransactionConsensus);
  add_rule(rules, index, state, EventKind::ConsensusReached,
           GuardId::OnPriorEchoAttemptsRemain, ActionId::RetryWithoutPromotion,
           NextStateId::ResponseTailQuarantine, 3, TemplateOrigin::TransactionConsensus);
  add_rule(rules, index, state, EventKind::ConsensusReached,
           GuardId::RemainingMismatchAttemptsRemain, ActionId::ApplyMismatchWithRetry,
           NextStateId::ResponseTailQuarantine, 4, TemplateOrigin::TransactionConsensus);
  add_rule(rules, index, state, EventKind::ConsensusReached,
           GuardId::RemainingMismatchExhausted, ActionId::ExhaustMismatch,
           NextStateId::ResponseTailQuarantine, 5, TemplateOrigin::TransactionConsensus);
}

// Computed is reserved for handlers whose target depends on runtime evidence:
// candidate tracking may iteratively continue to consensus; recovery misses and due
// events consult scheduler state; restore, first-ready, command, and timer events
// inspect persisted or live context; holdoff/tail exits inspect deferred work;
// learning depends on observed frames or sender presence; and radio recovery
// selects a target from the interrupted lease/TX context and retry outcome.
constexpr std::array<TransitionRule, kRuleCount> make_rules() {
  std::array<TransitionRule, kRuleCount> rules{};
  std::size_t index = 0;
  for (std::size_t family = 0; family < 4; ++family)
    add_query_family(rules, index, family);
  add_rule(rules, index, CoordinatorState::Idle,
           EventKind::PassiveResponseOnlyCandidateHeard, GuardId::Always,
           ActionId::HandlePassiveCandidate, NextStateId::Same, 1,
           TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::Idle,
           EventKind::PassiveAmbiguousCandidateHeard, GuardId::Always,
           ActionId::HandlePassiveCandidate, NextStateId::Computed, 1,
           TemplateOrigin::None);
  for (const auto family : {0U, 1U, 3U}) {
    add_rule(rules, index, kQueryStates[family].response,
             EventKind::ConsensusReached, GuardId::Always,
             ActionId::ApplyConsensus, NextStateId::ResponseTailQuarantine, 1,
             TemplateOrigin::None);
  }
  add_consensus(rules, index, CoordinatorState::PostCommandListening);
  add_consensus(rules, index, CoordinatorState::FallbackResponseListening);
  for (std::uint8_t value = 0; value < kCoordinatorStateCount; ++value) {
    const auto state = static_cast<CoordinatorState>(value);
    add_rule(rules, index, state, EventKind::ManualRefreshRequested, GuardId::Always,
             state == CoordinatorState::Idle ? ActionId::AcceptRefresh
                                             : ActionId::RefuseRefresh,
             state == CoordinatorState::Idle ? NextStateId::ManualQueryPending
                                             : NextStateId::Same,
             1, TemplateOrigin::None);
    add_rule(rules, index, state, EventKind::HeartbeatRequested,
             GuardId::Always, ActionId::HandleHeartbeatRequest,
             NextStateId::Computed, 1, TemplateOrigin::None);
  }
  for (std::uint8_t value = 0; value < kCoordinatorStateCount; ++value) {
    const auto state = static_cast<CoordinatorState>(value);
    const bool learning = state == CoordinatorState::LearningAwaitingFirst ||
                          state == CoordinatorState::LearningAwaitingSecond;
    const bool unprovisioned = state == CoordinatorState::Unprovisioned;
    add_rule(rules, index, state, EventKind::ExactOemQueryHeard, GuardId::Always,
             learning || unprovisioned ? ActionId::IgnoreLearningOem
                                       : ActionId::AssertOemPriority,
             learning || unprovisioned ? NextStateId::Same : NextStateId::OemHoldoff,
             1, TemplateOrigin::None);
  }
  add_rule(rules, index, CoordinatorState::PostCommandTailWait,
           EventKind::TailExpired, GuardId::TailOwnsTransaction,
           ActionId::CreateFallback, NextStateId::FallbackQueryPending, 1,
           TemplateOrigin::None);
  for (std::uint8_t value = 0; value < kCoordinatorStateCount; ++value) {
    const auto state = static_cast<CoordinatorState>(value);
    // Restore depends on persisted validity, RadioReady on first-ready history, and
    // CommandRequested on transaction/window context, so their targets are computed.
    add_rule(rules, index, state, EventKind::ProvisioningRestored, GuardId::Always,
             ActionId::HandleRestore, NextStateId::Computed, 1, TemplateOrigin::None);
    add_rule(rules, index, state, EventKind::RadioReady, GuardId::Always,
             ActionId::HandleRadioReady, NextStateId::Computed, 1, TemplateOrigin::None);
    add_rule(rules, index, state, EventKind::CommandRequested, GuardId::Always,
             ActionId::HandleCommandRequest, NextStateId::Computed, 1, TemplateOrigin::None);
    // Computed, not a fixed LearningAwaitingFirst: handle_learn refuses when a
    // sender is already bound (issue #16) and must leave the current state
    // untouched; a fixed next-state here would overwrite the refusal into a
    // learn window. On the accepted (unprovisioned) path handle_learn sets
    // LearningAwaitingFirst itself, byte-for-byte as before.
    add_rule(rules, index, state, EventKind::LearnRequested, GuardId::Always,
             ActionId::HandleLearnRequest, NextStateId::Computed, 1,
             TemplateOrigin::None);
    add_rule(rules, index, state, EventKind::ForgetRequested, GuardId::Always,
             ActionId::HandleForget, NextStateId::Unprovisioned, 1, TemplateOrigin::None);
  }
  for (std::uint8_t value = 0; value < kCoordinatorStateCount; ++value) {
    const auto state = static_cast<CoordinatorState>(value);
    const bool query_response = state == CoordinatorState::BootResponseListening ||
        state == CoordinatorState::ManualResponseListening ||
        state == CoordinatorState::FallbackResponseListening ||
        state == CoordinatorState::RecoveryResponseListening;
    if (!query_response)
      add_rule(rules, index, state, EventKind::ExternalStateHeard, GuardId::Always,
               ActionId::HandleExternalState, NextStateId::OemHoldoff, 1,
               TemplateOrigin::None);
  }
  // A 0xCE special response is surfaced as observable protocol evidence and
  // nothing more. PublishDiagnostic emits CoreEventKind::Diagnostic; Same means
  // no state, authority, timer, or TX change and no fan actuation, in every
  // state — this event is deliberately never an input to authority.
  for (std::uint8_t value = 0; value < kCoordinatorStateCount; ++value)
    add_rule(rules, index, static_cast<CoordinatorState>(value),
             EventKind::SpecialDiagnosticHeard, GuardId::Always,
             ActionId::PublishDiagnostic, NextStateId::Same, 1,
             TemplateOrigin::None);
  for (std::uint8_t value = 0; value < kCoordinatorStateCount; ++value)
    add_rule(rules, index, static_cast<CoordinatorState>(value),
             EventKind::TimerEstimateExpired, GuardId::Always,
             ActionId::HandleTimerExpiry, NextStateId::Computed, 1,
             TemplateOrigin::None);
  // Computed, not a fixed CommandLeaseIssued: issue_command owns its resulting
  // state so an encode-failure refusal can return to a coherent terminal (Idle)
  // instead of being overwritten into a CommandLeaseIssued/CommandPendingContext
  // wedge. Success still sets CommandLeaseIssued itself, byte-for-byte unchanged.
  add_rule(rules, index, CoordinatorState::CommandPending, EventKind::Poll,
           GuardId::CanLease, ActionId::IssueCommandLease,
           NextStateId::Computed, 1, TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::CommandLeaseIssued,
           EventKind::TxBurstStarted, GuardId::MatchingToken, ActionId::StartCommand,
           NextStateId::CommandTransmitting, 1, TemplateOrigin::None);
  for (const auto event : {EventKind::TxBurstRejected,
                           EventKind::TxLeaseWatchdogFired})
    add_rule(rules, index, CoordinatorState::CommandLeaseIssued, event,
             GuardId::MatchingToken, ActionId::BeginRadioRecovery,
             NextStateId::RadioRecovery, 1, TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::CommandTransmitting,
           EventKind::TxBurstComplete, GuardId::MatchingToken,
           ActionId::CompleteCommand, NextStateId::PostCommandListening, 1,
           TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::CommandTransmitting,
           EventKind::TxBurstWatchdogFired, GuardId::MatchingToken,
           ActionId::BeginRadioRecovery, NextStateId::RadioRecovery, 1,
           TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::CommandTransmitting,
           EventKind::TxBurstFault, GuardId::MatchingToken,
           ActionId::BeginRadioRecovery, NextStateId::RadioRecovery, 1,
           TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::PostCommandListening,
           EventKind::IgnoredPostCommandPreAcceptanceState, GuardId::Always,
           ActionId::PublishDiagnostic, NextStateId::Same, 1,
           TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::PostCommandListening,
           EventKind::ResponseCandidate, GuardId::CandidateInEpoch,
           ActionId::TrackCandidate, NextStateId::Computed, 1,
           TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::PostCommandListening,
           EventKind::WindowExpired, GuardId::Always,
           ActionId::HandlePostWindowMiss, NextStateId::PostCommandTailWait, 1,
           TemplateOrigin::None);
  for (const auto event : {EventKind::LocalTailRepeat,
                           EventKind::LocalTailContradiction}) {
    add_rule(rules, index, CoordinatorState::PostCommandListening, event,
             GuardId::Always, ActionId::HandleTailFrame,
             NextStateId::PostCommandTailWait, 1, TemplateOrigin::None);
    add_rule(rules, index, CoordinatorState::PostCommandTailWait, event,
             GuardId::Always, ActionId::HandleTailFrame, NextStateId::Same, 1,
             TemplateOrigin::None);
    add_rule(rules, index, CoordinatorState::ResponseTailQuarantine, event,
             GuardId::Always, ActionId::HandleTailFrame, NextStateId::Same, 1,
             TemplateOrigin::None);
  }
  add_rule(rules, index, CoordinatorState::RetryDelay, EventKind::RetryDue,
           GuardId::Always, ActionId::HandleRetryDue,
           NextStateId::CommandPending, 1,
           TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::OemHoldoff,
           EventKind::OemHoldoffExpired, GuardId::Always,
           ActionId::HandleHoldoffExpired, NextStateId::Computed, 1,
           TemplateOrigin::None);
  for (const auto state : {CoordinatorState::RecoveryQuietWait,
                           CoordinatorState::RecoveryRetryWait})
    add_rule(rules, index, state, EventKind::RecoveryDue, GuardId::Always,
             ActionId::HandleRecoveryDue, NextStateId::Computed, 1,
             TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::ResponseTailQuarantine,
           EventKind::TailExpired, GuardId::TailBeginsRecoveryRetry,
           ActionId::HandleTailFrame, NextStateId::RecoveryRetryWait, 1,
           TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::ResponseTailQuarantine,
           EventKind::TailExpired, GuardId::Always, ActionId::HandleTailFrame,
           NextStateId::Computed, 2, TemplateOrigin::None);
  for (const auto state : {CoordinatorState::LearningAwaitingFirst,
                           CoordinatorState::LearningAwaitingSecond}) {
    add_rule(rules, index, state, EventKind::FrameReceived, GuardId::Always,
             ActionId::HandleLearningFrame, NextStateId::Computed, 1,
             TemplateOrigin::None);
    add_rule(rules, index, state, EventKind::LearnWindowExpired, GuardId::Always,
             ActionId::HandleLearnExpiry, NextStateId::Computed, 1,
             TemplateOrigin::None);
  }
  add_rule(rules, index, CoordinatorState::RadioRecovery,
           EventKind::RadioRecovered, GuardId::Always,
           ActionId::HandleRadioRecovered, NextStateId::Computed, 1,
           TemplateOrigin::None);
  add_rule(rules, index, CoordinatorState::RadioRecovery,
           EventKind::TxBurstWatchdogFired, GuardId::Always,
           ActionId::BeginRadioRecovery, NextStateId::Computed, 1,
           TemplateOrigin::None);
  return rules;
}

constexpr auto kRules = make_rules();

constexpr bool valid_rules() {
  constexpr std::size_t kEventCount =
      static_cast<std::size_t>(EventKind::Poll) + 1U;
  std::array<std::uint16_t, kCoordinatorStateCount * kEventCount> last_priority{};
  for (std::size_t i = 0; i < kRules.size(); ++i) {
    const auto& rule = kRules[i];
    if (rule.rule_id != i + 1U) return false;
    const std::size_t key = static_cast<std::size_t>(rule.state) * kEventCount +
                            static_cast<std::size_t>(rule.event);
    if (last_priority[key] != 0 && last_priority[key] >= rule.priority)
      return false;
    last_priority[key] = rule.priority;
  }
  return true;
}
static_assert(valid_rules(), "transition rules must have unique IDs and priorities");

// The reducer relies on this: TrackCandidate returns to the loop and lets the
// continuation's rule choose the state, so a fixed next-state on those rules
// would be silently discarded.
constexpr bool track_candidate_rules_are_computed() {
  for (const auto& rule : kRules)
    if (rule.action == ActionId::TrackCandidate &&
        rule.next != NextStateId::Computed)
      return false;
  return true;
}
static_assert(track_candidate_rules_are_computed(),
              "TrackCandidate rules must declare NextStateId::Computed");

}  // namespace

TransitionRuleView TransitionTable::rules() { return {kRules.data(), kRules.size()}; }

std::optional<CoordinatorState> TransitionTable::fixed_next_state(
    NextStateId next, CoordinatorState current) {
  switch (next) {
    case NextStateId::Same:return current;
    case NextStateId::Computed:return std::nullopt;
    case NextStateId::Unprovisioned:return CoordinatorState::Unprovisioned;
    case NextStateId::Idle:return CoordinatorState::Idle;
    case NextStateId::CommandPending:return CoordinatorState::CommandPending;
    case NextStateId::CommandLeaseIssued:return CoordinatorState::CommandLeaseIssued;
    case NextStateId::CommandTransmitting:return CoordinatorState::CommandTransmitting;
    case NextStateId::PostCommandListening:return CoordinatorState::PostCommandListening;
    case NextStateId::PostCommandTailWait:return CoordinatorState::PostCommandTailWait;
    case NextStateId::FallbackQueryPending:return CoordinatorState::FallbackQueryPending;
    case NextStateId::FallbackQueryLeaseIssued:return CoordinatorState::FallbackQueryLeaseIssued;
    case NextStateId::FallbackQueryTransmitting:return CoordinatorState::FallbackQueryTransmitting;
    case NextStateId::FallbackResponseListening:return CoordinatorState::FallbackResponseListening;
    case NextStateId::RetryDelay:return CoordinatorState::RetryDelay;
    case NextStateId::BootQueryPending:return CoordinatorState::BootQueryPending;
    case NextStateId::BootQueryLeaseIssued:return CoordinatorState::BootQueryLeaseIssued;
    case NextStateId::BootQueryTransmitting:return CoordinatorState::BootQueryTransmitting;
    case NextStateId::BootResponseListening:return CoordinatorState::BootResponseListening;
    case NextStateId::ManualQueryPending:return CoordinatorState::ManualQueryPending;
    case NextStateId::ManualQueryLeaseIssued:return CoordinatorState::ManualQueryLeaseIssued;
    case NextStateId::ManualQueryTransmitting:return CoordinatorState::ManualQueryTransmitting;
    case NextStateId::ManualResponseListening:return CoordinatorState::ManualResponseListening;
    case NextStateId::OemHoldoff:return CoordinatorState::OemHoldoff;
    case NextStateId::RecoveryQuietWait:return CoordinatorState::RecoveryQuietWait;
    case NextStateId::RecoveryQueryPending:return CoordinatorState::RecoveryQueryPending;
    case NextStateId::RecoveryQueryLeaseIssued:return CoordinatorState::RecoveryQueryLeaseIssued;
    case NextStateId::RecoveryQueryTransmitting:return CoordinatorState::RecoveryQueryTransmitting;
    case NextStateId::RecoveryResponseListening:return CoordinatorState::RecoveryResponseListening;
    case NextStateId::RecoveryRetryWait:return CoordinatorState::RecoveryRetryWait;
    case NextStateId::ResponseTailQuarantine:return CoordinatorState::ResponseTailQuarantine;
    case NextStateId::LearningAwaitingFirst:return CoordinatorState::LearningAwaitingFirst;
    case NextStateId::LearningAwaitingSecond:return CoordinatorState::LearningAwaitingSecond;
    case NextStateId::RadioRecovery:return CoordinatorState::RadioRecovery;
  }
  return std::nullopt;
}

std::size_t TransitionTable::query_template_rule_count(TemplateOrigin origin) {
  std::size_t count = 0;
  for (const auto& rule : kRules) if (rule.origin == origin) ++count;
  return count;
}

RfPermission TransitionTable::rf_permission(CoordinatorState state) {
  const bool command = state == CoordinatorState::CommandPending;
  const bool query = state == CoordinatorState::BootQueryPending ||
      state == CoordinatorState::ManualQueryPending ||
      state == CoordinatorState::FallbackQueryPending ||
      state == CoordinatorState::RecoveryQueryPending;
  const bool response = state == CoordinatorState::PostCommandListening ||
      state == CoordinatorState::FallbackResponseListening ||
      state == CoordinatorState::BootResponseListening ||
      state == CoordinatorState::ManualResponseListening ||
      state == CoordinatorState::RecoveryResponseListening;
  const bool tail = response || state == CoordinatorState::PostCommandTailWait ||
      state == CoordinatorState::ResponseTailQuarantine;
  return {command, query, response, tail};
}

bool TransitionTable::refresh_is_accepted(CoordinatorState state) {
  return state == CoordinatorState::Idle;
}

CoordinatorState TransitionTable::query_lease_state(QueryPurpose purpose) {
  return kQueryStates[query_family_index(purpose)].lease;
}

CoordinatorState TransitionTable::query_transmitting_state(QueryPurpose purpose) {
  return kQueryStates[query_family_index(purpose)].transmitting;
}

CoordinatorState TransitionTable::query_response_state(QueryPurpose purpose) {
  return kQueryStates[query_family_index(purpose)].response;
}

CoordinatorState TransitionTable::query_pending_state(QueryPurpose purpose) {
  return kQueryStates[query_family_index(purpose)].pending;
}

QueryPurpose TransitionTable::query_purpose(TxReason reason) {
  if (reason == TxReason::BootQuery) return QueryPurpose::Boot;
  if (reason == TxReason::ManualQuery) return QueryPurpose::Manual;
  if (reason == TxReason::HeartbeatQuery) return QueryPurpose::Heartbeat;
  if (reason == TxReason::TransactionFallbackQuery) return QueryPurpose::Fallback;
  return QueryPurpose::Recovery;
}

std::optional<QueryPurpose> TransitionTable::query_family_of(TxReason reason) {
  // No `default:` on purpose: -Wswitch -Werror forces a new TxReason to be
  // classified here explicitly, rather than silently folding into a family.
  switch (reason) {
  case TxReason::BootQuery:
    return QueryPurpose::Boot;
  case TxReason::ManualQuery:
    return QueryPurpose::Manual;
  case TxReason::HeartbeatQuery:
    return QueryPurpose::Heartbeat;
  case TxReason::TransactionFallbackQuery:
    return QueryPurpose::Fallback;
  case TxReason::RecoveryQueryInitial:
  case TxReason::RecoveryQueryRetry:
  case TxReason::TimerExpiryRecoveryQuery:
    return QueryPurpose::Recovery;
  case TxReason::TransactionCommand:
    return std::nullopt;  // a command TX reason, not a query reason
  }
  return std::nullopt;  // unreachable: the switch is exhaustive over TxReason
}

}  // namespace quietcool
