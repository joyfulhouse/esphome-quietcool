#pragma once

#include "core_types.h"

namespace quietcool {

enum class EventKind : std::uint8_t {
  ProvisioningRestored, RadioReady, CommandRequested, ManualRefreshRequested,
  LearnRequested, ForgetRequested, FrameReceived, TxBurstStarted,
  TxBurstComplete, TxBurstRejected, TxBurstFault, RadioRecovered,
  ExactOemQueryHeard,
  ExternalStateHeard, IgnoredPostCommandPreAcceptanceState, ResponseCandidate,
  ConsensusReached, WindowExpired,
  TailExpired, RetryDue, OemHoldoffExpired, RecoveryDue,
  LearnCandidateStarted, Learned, LearnWindowExpired, TimerEstimateExpired,
  TxLeaseWatchdogFired, TxBurstWatchdogFired, LocalTailRepeat,
  LocalTailContradiction, SpecialDiagnosticHeard, Poll
};
enum class GuardId : std::uint8_t {
  Always, CanLease, MatchingToken, CandidateInEpoch, SemanticMatch,
  OnCommandMismatchNotPrior, OnPriorEchoAttemptsRemain,
  RemainingMismatchAttemptsRemain, RemainingMismatchExhausted,
  TailOwnsTransaction, TailBeginsRecoveryRetry
};
enum class ActionId : std::uint8_t {
  None, IssueQueryLease, StartQuery, BeginRadioRecovery, OpenQueryResponse,
  TrackCandidate, ApplyConsensus, FinishQueryMiss, AssertOemPriority,
  IgnoreLearningOem, AcceptRefresh, RefuseRefresh, ConfirmAndPromote,
  YieldToOem, RetryWithoutPromotion, ApplyMismatchWithRetry,
  ExhaustMismatch, CreateFallback, HandleRestore, HandleRadioReady,
  HandleCommandRequest, HandleLearnRequest, HandleForget,
  HandleExternalState, HandleTimerExpiry, IssueCommandLease,
  StartCommand, CompleteCommand, HandlePostWindowMiss, HandleTailFrame,
  HandleRetryDue, HandleHoldoffExpired, HandleRecoveryDue,
  HandleLearningFrame, HandleLearnExpiry, HandleRadioRecovered,
  PublishDiagnostic, InvalidInternalEvent
};
enum class NextStateId : std::uint8_t {
  Same, Computed, Unprovisioned, Idle, CommandPending, CommandLeaseIssued,
  CommandTransmitting, PostCommandListening, PostCommandTailWait,
  FallbackQueryPending, FallbackQueryLeaseIssued,
  FallbackQueryTransmitting, FallbackResponseListening, RetryDelay,
  BootQueryPending, BootQueryLeaseIssued, BootQueryTransmitting,
  BootResponseListening, ManualQueryPending, ManualQueryLeaseIssued,
  ManualQueryTransmitting, ManualResponseListening, OemHoldoff,
  RecoveryQuietWait, RecoveryQueryPending, RecoveryQueryLeaseIssued,
  RecoveryQueryTransmitting, RecoveryResponseListening, RecoveryRetryWait,
  ResponseTailQuarantine, LearningAwaitingFirst, LearningAwaitingSecond,
  RadioRecovery
};
enum class TemplateOrigin : std::uint8_t {
  None, BootQuery, ManualQuery, FallbackQuery, RecoveryQuery,
  TransactionConsensus
};

struct TransitionRule final {
  CoordinatorState state;
  EventKind event;
  GuardId guard;
  ActionId action;
  NextStateId next;
  std::uint16_t priority;
  std::uint16_t rule_id;
  TemplateOrigin origin;
};
struct TransitionRuleView final {
  const TransitionRule* data;
  std::size_t size;
};
enum class PriorRelation : std::uint8_t { Absent, Equal, Unequal };
struct TransactionConsensusInput final {
  bool semantic_match;
  bool request_on;
  bool consensus_on;
  bool command_marker;
  PriorRelation prior_relation;
  bool attempts_remain;
};
struct TransactionRuleMatches final {
  TransitionRule first;
  std::size_t count;
};
struct RfPermission final {
  bool lease_command;
  bool lease_query;
  bool accept_response;
  bool classify_tail;
};

class TransitionTable final {
 public:
  static TransitionRuleView rules();
  static std::optional<CoordinatorState> fixed_next_state(
      NextStateId next, CoordinatorState current);
  static std::size_t query_template_rule_count(TemplateOrigin origin);
  static RfPermission rf_permission(CoordinatorState state);
  static bool refresh_is_accepted(CoordinatorState state);
  static CoordinatorState query_lease_state(QueryPurpose purpose);
  static CoordinatorState query_transmitting_state(QueryPurpose purpose);
  static CoordinatorState query_response_state(QueryPurpose purpose);
  static CoordinatorState query_pending_state(QueryPurpose purpose);
  static QueryPurpose query_purpose(TxReason reason);
};

}  // namespace quietcool
