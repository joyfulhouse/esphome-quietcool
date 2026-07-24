#pragma once

#include "command_transaction.h"
#include "consensus_tracker.h"
#include "sender_id.h"

namespace quietcool {

struct RestoredObservationHint final {
  std::uint8_t canonical_state;
  SpeedCapability capability;
};
enum class SeedPolicy : std::uint8_t { AllowCompiledSeed, SuppressCompiledSeed };
constexpr std::uint16_t kRestoreSchemaVersion = 1;
struct RestorableState final {
  std::uint16_t version{kRestoreSchemaVersion};
  std::optional<SenderId> sender;
  SeedPolicy seed_policy{SeedPolicy::AllowCompiledSeed};
  std::optional<Speed> remembered_speed;
  std::optional<RestoredObservationHint> observation_hint;
};
bool restorable_state_is_valid(const RestorableState& restored);
struct PersistenceRequest final {
  PersistenceKind kind;
  std::optional<SenderId> sender;
  std::optional<Speed> remembered_speed;
};
enum class TimerExpiryStatus : std::uint8_t { NotDue, Due };
struct TimerExpiryDecision final {
  TimerExpiryStatus status;
  std::optional<PersistenceRequest> persistence;
};

struct UnknownStateAuthority final {
  AuthorityLossReason reason;
  MonotonicMs since_ms;
  std::optional<FanState> last_diagnostic;
  std::optional<RestoredObservationHint> restored_hint;
};
struct RevalidatingStateAuthority final {
  std::optional<PriorAuthoritySnapshot> previous;
  MonotonicMs requested_ms;
  std::optional<TxToken> token;
};
struct ConfirmedStateAuthority final {
  FanState state;
  EvidenceSource source;
  EvidenceConfidence confidence;
  MonotonicMs observed_ms;
  std::uint8_t independent_candidates;
  std::optional<TransactionId> transaction;
  std::optional<AttemptNumber> attempt;
  std::uint64_t revision;
};
using StateAuthority = std::variant<UnknownStateAuthority,
                                    RevalidatingStateAuthority,
                                    ConfirmedStateAuthority>;

enum class TimerLossReason : std::uint8_t {
  Unknown, StateInvalidated, EstimatedDeadline, RestoredUnverified
};
enum class RemainingTimeStatus : std::uint8_t { Unknown };
enum class TimerAnchorPolicy : std::uint8_t { CommandBurstCompletion };
struct UnknownTimerAuthority final { TimerLossReason reason; MonotonicMs since_ms; };
struct NoActiveTimerAuthority final {
  bool confirmed_off;
  EvidenceSource source;
  MonotonicMs observed_ms;
};
struct ProgrammedDurationAuthority final {
  Duration duration;
  EvidenceSource source;
  MonotonicMs observed_ms;
  RemainingTimeStatus remaining{RemainingTimeStatus::Unknown};
};
struct LocallyAnchoredTimerAuthority final {
  Duration duration;
  TransactionId transaction;
  AttemptNumber attempt;
  MonotonicMs anchor_ms;
  MonotonicMs expiry_ms;
  EvidenceSource source;
  TimerAnchorPolicy policy{TimerAnchorPolicy::CommandBurstCompletion};
};
using TimerAuthority = std::variant<UnknownTimerAuthority,
    NoActiveTimerAuthority, ProgrammedDurationAuthority,
    LocallyAnchoredTimerAuthority>;

struct AcceptedObservation final {
  FanState state;
  SpeedCapability capability;
  EvidenceSource source;
  EvidenceConfidence confidence;
  MonotonicMs observed_ms;
  std::uint8_t independent_candidates;
  std::optional<TransactionId> transaction;
  std::optional<AttemptNumber> attempt;
  std::optional<MonotonicMs> command_completed_ms;
  bool anchors_local_timer;
};
struct AuthoritySnapshot final {
  StateAuthority state;
  TimerAuthority timer;
  std::optional<Speed> remembered_speed;
  std::optional<FanState> last_diagnostic;
  std::uint64_t revision;
};

class AuthorityStore final {
 public:
  AuthorityStore();
  AuthoritySnapshot snapshot(MonotonicMs now_ms) const;
  std::optional<PriorAuthoritySnapshot> capture_prior(MonotonicMs now_ms) const;
  void begin_local_command(TransactionId transaction, MonotonicMs now_ms);
  void begin_manual_revalidation(MonotonicMs now_ms);
  void bind_manual_query_token(TxToken token);
  void promote(const AcceptedObservation& accepted, MonotonicMs now_ms);
  void invalidate(AuthorityLossReason reason, MonotonicMs now_ms);
  void record_diagnostic(FanState state);
  TimerExpiryDecision timer_estimate_expired(MonotonicMs now_ms);
  std::optional<MonotonicMs> timer_deadline() const;
  void restore_hint(const RestorableState& restored, MonotonicMs now_ms);
 private:
  void set_unknown(AuthorityLossReason reason, TimerLossReason timer_reason,
                   MonotonicMs now_ms);
  StateAuthority state_;
  TimerAuthority timer_;
  std::optional<Speed> remembered_speed_;
  std::optional<FanState> last_diagnostic_;
  std::optional<RestoredObservationHint> restored_hint_;
  std::uint64_t revision_{0};
};

}  // namespace quietcool
