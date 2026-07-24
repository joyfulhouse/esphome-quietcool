#pragma once

#include "authority_store.h"
#include "learn_machine.h"
#include "observation_policy.h"
#include "recovery_scheduler.h"
#include "response_classifier.h"
#include "transition_table.h"

namespace quietcool {

struct CoreConfig final { std::uint32_t jitter_seed; };
struct RequestTxBurst final { TxRequest request; };
struct RevokeTxLease final { TxToken token; };
struct PublishCoreEvent final { CoreEvent event; };
struct RequestPersistenceEffect final { PersistenceRequest request; };
struct PublishAuthorityEffect final { AuthoritySnapshot authority; };
struct RequestRadioReset final { std::uint8_t attempt; };
struct RefusedInput final { RefusalReason reason; CoordinatorState state; };
using CoreEffect = std::variant<RequestTxBurst, RevokeTxLease, PublishCoreEvent,
    RequestPersistenceEffect, PublishAuthorityEffect, RequestRadioReset,
    RefusedInput>;

class CoreEffects final {
 public:
  // Production branches emit at most three effects in one reduction. Keep one
  // spare slot without paying for the former eight-slot buffer in every frame
  // that returns CoreEffects by value.
  static constexpr std::size_t kCapacity = 4;

  std::size_t size() const { return size_; }
  const CoreEffect& operator[](std::size_t index) const { return effects_[index].value(); }
  bool add(CoreEffect effect) {
    if (size_ == effects_.size()) return false;
    effects_[size_++] = std::move(effect);
    return true;
  }
 private:
  std::array<std::optional<CoreEffect>, kCapacity> effects_{};
  std::size_t size_{0};
};

struct QueuedCoreEffect final {
  CoreEffect effect;
  MonotonicMs now_ms;
};

class CoreEffectQueue final {
 public:
  static constexpr std::size_t kCapacity = 16;

  std::size_t size() const { return size_; }
  bool enqueue(const CoreEffects& effects, MonotonicMs now_ms) {
    if (effects.size() > kCapacity - size_) return false;
    for (std::size_t index = 0; index < effects.size(); ++index) {
      effects_[tail_] = QueuedCoreEffect{effects[index], now_ms};
      tail_ = (tail_ + 1U) % kCapacity;
      ++size_;
    }
    return true;
  }
  std::optional<QueuedCoreEffect> pop() {
    if (size_ == 0) return std::nullopt;
    auto effect = std::move(effects_[head_]);
    effects_[head_].reset();
    head_ = (head_ + 1U) % kCapacity;
    --size_;
    return effect;
  }

 private:
  std::array<std::optional<QueuedCoreEffect>, kCapacity> effects_{};
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t size_{0};
};

class CoreEffectDrain final {
 public:
  bool enqueue(const CoreEffects& effects, MonotonicMs now_ms) {
    if (!queue_.enqueue(effects, now_ms)) return false;
    latest_ms_ = now_ms;
    publication_pending_ = true;
    return true;
  }

  template <typename Apply, typename Publish>
  void drain(Apply& apply, Publish& publish) {
    if (draining_) return;
    draining_ = true;
    while (publication_pending_ || queue_.size() != 0) {
      while (const auto queued = queue_.pop()) {
        apply(queued->effect, queued->now_ms);
      }
      publication_pending_ = false;
      publish(latest_ms_);
    }
    draining_ = false;
  }

  bool draining() const { return draining_; }
  bool empty() const {
    return queue_.size() == 0 && !publication_pending_;
  }

 private:
  CoreEffectQueue queue_;
  MonotonicMs latest_ms_{0};
  bool publication_pending_{false};
  bool draining_{false};
};

struct UnprovisionedContext final {};
struct IdleContext final {};
struct CommandPendingContext final { MonotonicMs earliest_tx_ms; };
struct CommandLeaseContext final { TxToken token; MonotonicMs leased_ms; };
struct CommandTxContext final {
  TransactionId transaction; AttemptNumber attempt; TxToken token;
  MonotonicMs lease_ms; MonotonicMs started_ms;
};
struct PostCommandContext final {
  TransactionId transaction; AttemptNumber attempt; MonotonicMs completed_ms;
  std::uint32_t epoch_identity;
};
struct PostCommandTailContext final {
  TransactionId transaction; AttemptNumber attempt; std::uint32_t epoch_identity;
};
struct FallbackQueryContext final { TransactionId transaction; AttemptNumber attempt; };
struct QueryPendingContext final { QueryPurpose purpose; TxReason reason; };
struct QueryLeaseContext final {
  QueryPurpose purpose; TxReason reason; TxToken token; MonotonicMs leased_ms;
};
struct QueryTxContext final {
  QueryPurpose purpose; TxReason reason; TxToken token; MonotonicMs started_ms;
};
struct QueryResponseContext final {
  QueryPurpose purpose; TxReason reason; TxToken token; std::uint32_t epoch_identity;
};
struct RetryDelayContext final { MonotonicMs due_ms; };
struct OemHoldoffContext final { MonotonicMs latest_activity_ms; };
struct RecoveryContext final { RecoveryCause cause; };
struct ReturnIdle final {};
struct BeginRetryForTransaction final { TransactionId transaction; };
struct BeginDeferredCommand final {};
struct BeginRecoveryQuietWait final {};
struct BeginRecoveryRetryWait final {};
using TailExit = std::variant<ReturnIdle, BeginRetryForTransaction,
                              BeginDeferredCommand, BeginRecoveryQuietWait,
                              BeginRecoveryRetryWait>;
struct TailQuarantineContext final {
  TailExit exit; std::uint32_t epoch_identity; FanState expected_state;
};
struct LearningContext final { LearnMode mode; MonotonicMs deadline_ms; };
enum class RadioRecoveryTarget : std::uint8_t {
  ReissueUnstartedCommandLease, QuarantineStartedCommandThenRetry,
  ReissueUnstartedQueryLease, FinishStartedQueryAsMiss, ReturnIdleUnknown
};
struct RadioRecoveryContext final {
  RadioRecoveryTarget target; std::uint8_t attempts; std::optional<TxReason> reason;
  std::optional<TxToken> token;
  std::optional<FanState> expected_state;
  MonotonicMs target_anchor_ms;
  MonotonicMs reset_requested_ms;
};
using StateContext = std::variant<UnprovisionedContext, IdleContext,
    CommandPendingContext, CommandLeaseContext, CommandTxContext,
    PostCommandContext, PostCommandTailContext, FallbackQueryContext,
    QueryPendingContext, QueryLeaseContext, QueryTxContext,
    QueryResponseContext, RetryDelayContext, OemHoldoffContext,
    RecoveryContext, TailQuarantineContext, LearningContext,
    RadioRecoveryContext>;

struct CoreSnapshot final {
  CoordinatorState state;
  StateContext context;
  AuthoritySnapshot authority;
  std::optional<TransactionSnapshot> transaction;
  RecoverySnapshot recovery;
  LearnSnapshot learning;
  std::optional<TxRequest> live_tx;
  std::optional<TransactionOutcome> last_transaction_outcome;
  std::optional<FanState> deferred_command;
  std::uint32_t epoch_identity;
  std::uint32_t logical_command_bursts;
  std::uint32_t logical_query_bursts;
};

#ifdef QUIETCOOL_HOST_TEST
class ConfirmationCoreTestBuilder;
#endif

class ConfirmationCore final {
 public:
  explicit ConfirmationCore(const CoreConfig& config);
  CoreEffects restore(const RestorableState& restored, MonotonicMs now_ms);
  CoreEffects on_radio_ready(MonotonicMs now_ms);
  CoreEffects request_state(FanState requested, MonotonicMs now_ms);
  CoreEffects request_manual_refresh(MonotonicMs now_ms);
  CoreEffects request_learn(LearnMode mode, MonotonicMs now_ms);
  CoreEffects request_forget(MonotonicMs now_ms);
  CoreEffects on_frame(ByteView input, MonotonicMs now_ms);
  CoreEffects on_tx_started(TxToken token, MonotonicMs now_ms);
  CoreEffects on_tx_complete(TxToken token, MonotonicMs now_ms);
  CoreEffects on_tx_rejected(TxToken token, MonotonicMs now_ms);
  CoreEffects on_tx_fault(TxToken token, MonotonicMs now_ms);
  CoreEffects on_radio_recovered(MonotonicMs now_ms);
  CoreEffects poll(MonotonicMs now_ms);
  CoreSnapshot snapshot(MonotonicMs now_ms) const;
  static StateContext context_for_test(CoordinatorState state);
  static bool context_matches_state(CoordinatorState state,
                                    const StateContext& context);
  static TransactionRuleMatches matching_transaction_rules_for_test(
      CoordinatorState state, const TransactionConsensusInput& input);
  static bool action_is_dispatchable_for_test(ActionId action);
 private:
#ifdef QUIETCOOL_HOST_TEST
  friend class ConfirmationCoreTestBuilder;
#endif
  struct ReducerInput final {
    ReducerInput(EventKind event_kind, MonotonicMs event_ms)
        : kind(event_kind), now_ms(event_ms) {}
    EventKind kind;
    MonotonicMs now_ms;
    const RestorableState* restored{nullptr};
    const FanState* requested{nullptr};
    const LearnMode* learn_mode{nullptr};
    const ByteView* frame{nullptr};
    const ClassifiedFrame* classified{nullptr};
    const Consensus* consensus{nullptr};
    std::optional<TxToken> token;
    std::optional<RecoveryDue> recovery_due;
    std::optional<MonotonicMs> timer_deadline;
  };
  static bool transaction_guard_matches(GuardId guard,
                                        const TransactionConsensusInput& input) {
    const bool yield=!input.semantic_match && input.request_on && input.consensus_on && input.command_marker && input.prior_relation!=PriorRelation::Equal;
    const bool stale=!input.semantic_match && input.request_on && input.consensus_on && input.command_marker && input.prior_relation==PriorRelation::Equal && input.attempts_remain;
    switch (guard) {
      case GuardId::SemanticMatch:return input.semantic_match;
      case GuardId::OnCommandMismatchNotPrior:return yield;
      case GuardId::OnPriorEchoAttemptsRemain:return stale;
      case GuardId::RemainingMismatchAttemptsRemain:return !input.semantic_match && input.attempts_remain && !yield && !stale;
      case GuardId::RemainingMismatchExhausted:return !input.semantic_match && !input.attempts_remain && !yield;
      default:return false;
    }
  }
  CoreEffects reduce(const ReducerInput& input);
  const TransitionRule* select_rule(const ReducerInput& input) const;
  bool guard_matches(GuardId guard, const ReducerInput& input) const;
  CoreEffects dispatch(ActionId action, const ReducerInput& input);
  CoreEffects handle_restore(const RestorableState& restored, MonotonicMs now_ms);
  CoreEffects handle_radio_ready(MonotonicMs now_ms);
  CoreEffects handle_command_request(FanState requested, MonotonicMs now_ms);
  CoreEffects handle_manual_refresh(ActionId action, MonotonicMs now_ms);
  CoreEffects handle_learn(LearnMode mode, MonotonicMs now_ms);
  CoreEffects handle_forget(MonotonicMs now_ms);
  CoreEffects handle_learning_frame(ByteView input, MonotonicMs now_ms);
  CoreEffects handle_tx_started(TxToken token, MonotonicMs now_ms);
  CoreEffects handle_tx_complete(TxToken token, MonotonicMs now_ms);
  CoreEffects handle_radio_fault(EventKind kind, std::optional<TxToken> token,
                                 MonotonicMs now_ms);
  CoreEffects handle_radio_recovered(MonotonicMs now_ms);
  CoreEffects handle_tail_frame(EventKind kind, MonotonicMs now_ms);
  CoreEffects handle_recovery_due(const RecoveryDue& due);
  CoreEffects handle_timer_expiry(MonotonicMs deadline, MonotonicMs now_ms);
  CoreEffects refuse(RefusalReason reason) const;
  CoreEffects assert_oem_priority(MonotonicMs now_ms, bool external_state);
  CoreEffects issue_command(MonotonicMs now_ms);
  CoreEffects issue_query(QueryPurpose purpose, TxReason reason, MonotonicMs now_ms);
  CoreEffects apply_consensus(const Consensus& consensus, ActionId action,
                              MonotonicMs now_ms);
  void promote_authority(const AcceptedObservation& accepted,
                         MonotonicMs now_ms, CoreEffects& effects);
  CoreEffects close_window_as_miss(MonotonicMs now_ms);
  CoreEffects expire_tail(MonotonicMs now_ms);
  FanState safe_tail_state() const;
  CoreEffects begin_transaction(FanState request, MonotonicMs now_ms,
      std::optional<PriorAuthoritySnapshot> prior, bool publish_accept = true);
  void defer_command(FanState request, std::optional<PriorAuthoritySnapshot> prior,
                     CoreEffects& effects);
  void clear_deferred();
  CoreEffects begin_deferred(MonotonicMs now_ms);
  void finish_transaction(TransactionOutcome outcome, CoreEffects& effects);
  void enter_tail(TailExit exit, FanState expected);
  ReceiveContext receive_context() const;
  bool token_matches(TxToken token) const;
  CoordinatorState state_{CoordinatorState::Unprovisioned};
  StateContext context_{UnprovisionedContext{}};
  std::optional<SenderId> sender_;
  std::optional<CommandTransaction> transaction_;
  std::optional<FanState> deferred_command_;
  std::optional<PriorAuthoritySnapshot> deferred_prior_;
  std::optional<ResponseWindow> window_;
  ConsensusTracker consensus_;
  AuthorityStore authority_;
  ObservationPolicy policy_;
  RecoveryScheduler recovery_;
  LearnMachine learn_;
  ResponseClassifier classifier_;
  std::optional<TxRequest> live_tx_;
  std::optional<MonotonicMs> last_command_completed_ms_;
  std::optional<TransactionOutcome> last_outcome_;
  MonotonicIdAllocator<TransactionId> transaction_ids_;
  MonotonicIdAllocator<TxToken> tx_tokens_;
  std::uint32_t epoch_identity_{0};
  std::uint32_t command_bursts_{0};
  std::uint32_t query_bursts_{0};
  bool radio_ready_seen_{false};
};

}  // namespace quietcool
