#pragma once

#include "authority_store.h"
#include "learn_machine.h"
#include "observation_policy.h"
#include "recovery_scheduler.h"
#include "ring_buffer.h"
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
  // Four is the exact production maximum, not a maximum plus slack: the
  // supersession-with-pre-existing-deferred path emits RequestRadioReset,
  // TransactionFinished, Superseded and RequestAccepted in one reduction, and
  // a confirming promote whose remembered speed AND speed capability both
  // change emits SaveRememberedSpeed, SaveSpeedCapability, PublishAuthority
  // and TransactionFinished (issue #31).
  // add() fails closed on the fifth, and callers ignore the return, so a new
  // branch that emits five would silently drop one — raise this if that ever
  // becomes possible. The buffer is deliberately small because CoreEffects is
  // returned by value through the deepest frames on the target stack.
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

  std::size_t size() const { return ring_.size(); }

  // All-or-nothing: a partially admitted batch would apply some of a
  // reduction's effects and drop the rest, which is worse than refusing it.
  bool enqueue(const CoreEffects& effects, MonotonicMs now_ms) {
    if (effects.size() > ring_.available()) return false;
    for (std::size_t index = 0; index < effects.size(); ++index)
      ring_.push(QueuedCoreEffect{effects[index], now_ms});
    return true;
  }
  std::optional<QueuedCoreEffect> pop() { return ring_.pop(); }

 private:
  RingBuffer<QueuedCoreEffect, kCapacity> ring_;
};

class CoreEffectDrain final {
 public:
  bool enqueue(const CoreEffects& effects, MonotonicMs now_ms) {
    if (!queue_.enqueue(effects, now_ms)) return false;
    latest_ms_ = now_ms;
    publication_pending_ = true;
    return true;
  }

  // Bounds one top-level drain so a re-entrant automation-feedback storm cannot
  // make loop() run forever (M1, issue #8). 32 = 2x the effect-queue capacity
  // (16): a legitimate drain applies at most one full queue's worth of
  // already-admitted effects — apply_effect never enqueues onto the effect
  // queue itself, only user automations re-enter — so this never trips on real
  // work while capping loop() to a small constant of shallow, non-recursive
  // apply() calls. Not safety-critical: any value strictly greater than
  // CoreEffectQueue::kCapacity works; 32 is headroom.
  static constexpr std::size_t kMaxAppliesPerDrain = 32;

  // Terminal, one-way stop (issue #22). halt() is what degrade() calls: once the
  // component is being marked failed, the drain must abandon the rest of the
  // batch, because any further apply() would run on a dead controller and could
  // overwrite the terminal "unavailable" publication (and queued authority /
  // persistence effects must not run either). This is DELIBERATELY the opposite
  // of budget exhaustion below: budget exhaustion is backpressure and preserves
  // every un-applied effect for the next loop() (issue #8, no-drop); halting is
  // terminal and discards the remainder. The two must never be conflated —
  // separate flag, separate exit — or one bricks the controller under ordinary
  // load and the other resurrects #22.
  void halt() { halted_ = true; }
  bool halted() const { return halted_; }

  template <typename Apply, typename Publish>
  void drain(Apply& apply, Publish& publish) {
    // Terminal across drains (issue #22): once halted, drain() never runs
    // again, so no later batch can publish over the terminal "unavailable".
    if (draining_ || halted_) return;
    draining_ = true;
    std::size_t applied = 0;
    bool budget_exhausted = false;
    while (publication_pending_ || queue_.size() != 0) {
      while (const auto queued = queue_.pop()) {
        apply(queued->effect, queued->now_ms);
        // degrade() may have halted us from inside apply(): stop before the next
        // apply and before this round's publish(), so nothing overruns the
        // terminal publication degrade() already made.
        if (halted_) break;
        if (++applied >= kMaxAppliesPerDrain) {
          budget_exhausted = true;
          break;
        }
      }
      if (halted_) break;
      // On budget exhaustion, break with the remainder still queued and
      // publication_pending_ untouched (still true for the pending round).
      // drain() is resumable — all state lives in queue_ / publication_pending_
      // / latest_ms_ — and every loop() re-enters it, so the leftovers get a
      // fresh budget next pass. No effect is dropped. This is backpressure, not
      // a fault: it is counted for diagnostics and must never mark the
      // component failed (contrast issue #9, where overflow is terminal).
      if (budget_exhausted) {
        ++budget_exhaustions_;
        break;
      }
      publication_pending_ = false;
      publish(latest_ms_);
    }
    draining_ = false;
  }

  bool draining() const { return draining_; }
  std::uint32_t budget_exhaustions() const { return budget_exhaustions_; }
  bool empty() const {
    return queue_.size() == 0 && !publication_pending_;
  }

 private:
  CoreEffectQueue queue_;
  MonotonicMs latest_ms_{0};
  std::uint32_t budget_exhaustions_{0};
  bool publication_pending_{false};
  bool draining_{false};
  bool halted_{false};  // terminal stop (issue #22); never set by the budget path
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
  bool passive_observation_pending;
  std::uint32_t passive_observation_epochs_opened;
  std::uint32_t passive_confirmations_promoted;
  std::uint32_t passive_epochs_cancelled_or_expired;
  std::uint32_t heartbeat_queries_admitted;
  std::uint32_t heartbeat_queries_skipped_busy;
  std::uint32_t heartbeat_queries_suppressed_recent;
  std::uint32_t heartbeat_misses;
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
  CoreEffects request_heartbeat(MonotonicMs now_ms,
                                MonotonicMs recent_interval_ms);
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
  // The authority store's monotonic revision, without a snapshot (issue #35).
  std::uint64_t authority_revision() const { return authority_.revision(); }
  static StateContext context_for_test(CoordinatorState state);
  static bool context_matches_state(CoordinatorState state,
                                    const StateContext& context);
  static TransactionRuleMatches matching_transaction_rules_for_test(
      CoordinatorState state, const TransactionConsensusInput& input);
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
    MonotonicMs heartbeat_interval_ms{0};
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
  CoreEffects handle_heartbeat_request(MonotonicMs recent_interval_ms,
                                       MonotonicMs now_ms);
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
  CoreEffects handle_passive_candidate(const RecoveredResponse& candidate,
                                       bool response_only,
                                       MonotonicMs now_ms);
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
  void cancel_passive_observation();
  void abandon_passive_observation(MonotonicMs now_ms);
  void expire_passive_evidence(MonotonicMs now_ms);
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
  struct PassiveObservationEpoch final {
    Consensus first_burst;
    MonotonicMs opened_ms;
    MonotonicMs last_first_burst_frame_ms;
  };
  std::optional<PassiveObservationEpoch> passive_observation_;
  std::uint32_t passive_observation_epochs_opened_{0};
  std::uint32_t passive_confirmations_promoted_{0};
  std::uint32_t passive_epochs_cancelled_or_expired_{0};
  std::uint32_t heartbeat_queries_admitted_{0};
  std::uint32_t heartbeat_queries_skipped_busy_{0};
  std::uint32_t heartbeat_queries_suppressed_recent_{0};
  std::uint32_t heartbeat_misses_{0};
  bool radio_ready_seen_{false};
};

}  // namespace quietcool
