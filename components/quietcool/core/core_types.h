#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <variant>

namespace quietcool {

using MonotonicMs = std::uint64_t;

template <typename Tag>
class OpaqueId final {
 public:
  constexpr explicit OpaqueId(std::uint64_t value) : value_(value) {}
  constexpr std::uint64_t value() const { return value_; }
  constexpr bool operator==(OpaqueId other) const { return value_ == other.value_; }
  constexpr bool operator!=(OpaqueId other) const { return !(*this == other); }
 private:
  std::uint64_t value_;
};

struct TransactionIdTag;
struct TxTokenTag;
struct AttemptNumberTag;
using TransactionId = OpaqueId<TransactionIdTag>;
using TxToken = OpaqueId<TxTokenTag>;
using AttemptNumber = OpaqueId<AttemptNumberTag>;

template <typename Id>
class MonotonicIdAllocator final {
 public:
  explicit constexpr MonotonicIdAllocator(std::uint64_t next = 1)
      : next_(next) {}
  std::optional<Id> allocate() {
    if (exhausted()) return std::nullopt;
    const auto value = next_;
    next_ = next_ == std::numeric_limits<std::uint64_t>::max() ? 0 : next_ + 1;
    return Id(value);
  }
  constexpr bool exhausted() const { return next_ == 0; }
  constexpr void reset() { next_ = 1; }
 private:
  std::uint64_t next_;
};

class RefireCount final {
 public:
  constexpr explicit RefireCount(std::uint8_t value) : value_(value) {}
  constexpr std::uint8_t value() const { return value_; }
  constexpr bool operator==(RefireCount other) const { return value_ == other.value_; }
 private:
  std::uint8_t value_;
};

template <typename T, typename E>
class Result final {
 public:
  static Result ok(T value) { return Result(std::in_place_index<0>, std::move(value)); }
  static Result err(E error) { return Result(std::in_place_index<1>, std::move(error)); }
  bool has_value() const { return value_.index() == 0; }
  explicit operator bool() const { return has_value(); }
  const T& value() const { return std::get<0>(value_); }
  T& value() { return std::get<0>(value_); }
  const E& error() const { return std::get<1>(value_); }
 private:
  template <std::size_t I, typename V>
  explicit Result(std::in_place_index_t<I> index, V&& value)
      : value_(index, std::forward<V>(value)) {}
  std::variant<T, E> value_;
};

class ByteView final {
 public:
  constexpr ByteView(const std::uint8_t* data, std::size_t size)
      : data_(data), size_(size) {}
  template <std::size_t N>
  constexpr explicit ByteView(const std::array<std::uint8_t, N>& bytes)
      : data_(bytes.data()), size_(N) {}
  constexpr const std::uint8_t* data() const { return data_; }
  constexpr std::size_t size() const { return size_; }
  constexpr std::uint8_t operator[](std::size_t index) const { return data_[index]; }
 private:
  const std::uint8_t* data_;
  std::size_t size_;
};

struct FrameBytes final {
  std::array<std::uint8_t, 6> bytes{};
  bool operator==(const FrameBytes& other) const { return bytes == other.bytes; }
};

enum class CoordinatorState : std::uint8_t {
  Unprovisioned, Idle, CommandPending, CommandLeaseIssued, CommandTransmitting,
  PostCommandListening, PostCommandTailWait, FallbackQueryPending,
  FallbackQueryLeaseIssued, FallbackQueryTransmitting, FallbackResponseListening,
  RetryDelay, BootQueryPending, BootQueryLeaseIssued, BootQueryTransmitting,
  BootResponseListening, ManualQueryPending, ManualQueryLeaseIssued,
  ManualQueryTransmitting, ManualResponseListening, OemHoldoff, RecoveryQuietWait,
  RecoveryQueryPending, RecoveryQueryLeaseIssued, RecoveryQueryTransmitting,
  RecoveryResponseListening, RecoveryRetryWait, ResponseTailQuarantine,
  LearningAwaitingFirst, LearningAwaitingSecond, RadioRecovery
};

// Derived from the enum rather than written down. The transition table's
// universal-rule generators loop over every state; a hand-maintained count
// would silently skip a newly added state, and nothing would fail.
constexpr std::size_t kCoordinatorStateCount =
    static_cast<std::size_t>(CoordinatorState::RadioRecovery) + 1U;

enum class TxReason : std::uint8_t {
  BootQuery, ManualQuery, RecoveryQueryInitial, RecoveryQueryRetry,
  TimerExpiryRecoveryQuery, HeartbeatQuery, TransactionCommand,
  TransactionFallbackQuery
};
enum class TransactionOutcome : std::uint8_t {
  Confirmed, Exhausted, Superseded, CancelledByExactOemQuery,
  CancelledByExternalState, YieldedToPossibleOemCommand, CancelledForLearning,
  RadioUnavailable
};
enum class RefusalReason : std::uint8_t { Unprovisioned, Busy, Holdoff, Learning, InvalidState, IdExhausted, AmbiguousLearn, AlreadyProvisioned };
enum class QueryPurpose : std::uint8_t {
  Boot, Manual, Fallback, Recovery, Heartbeat
};
enum class EvidenceSource : std::uint8_t {
  BootQueryConsensus, ManualQueryConsensus, RecoveryQueryConsensus,
  TimerExpiryRecoveryConsensus, PostCommandConsensus, FallbackQueryConsensus,
  PassiveObservationConsensus, HeartbeatQueryConsensus, ExternalDiagnostic,
  RestoredHint
};
enum class EvidenceConfidence : std::uint8_t { ExactBackedConsensus, RecoveredOnlyConsensus };
// How much a reported SpeedCapability may be trusted (issue #31 review). A
// command frame and a 2-speed fan's report are byte-identical by construction —
// both carry marker bits 10, which is also the encoding of SpeedCapability::Two
// — so a frame matching our own in-flight command is PossiblyOwnEcho: real
// evidence of a 2-speed fan, or no evidence at all if it was our own echo. The
// rank exists because the two failure modes are wildly asymmetric. Discarding
// such evidence makes capability Two unlearnable from a SUCCESSFUL command — a
// confirmed command never opens a fallback query, and outside a response window
// a report classifies as ExternalPriorityState, which invalidates rather than
// promotes — so a 2-speed fan driven only by commands would never confirm or
// persist its band, and every boot would fall back to the conservative
// unknown-capability assumption instead of to proof. Trusting it blindly is the
// worse half: an echo can then DEMOTE a fan already known to have three speeds,
// narrowing its entity to two levels across reboots and putting MED out of
// reach on a fan that has it. So PossiblyOwnEcho evidence is used, but never in
// preference to a frame that could not have been ours.
enum class CapabilityEvidence : std::uint8_t { Unambiguous, PossiblyOwnEcho };
enum class AuthorityLossReason : std::uint8_t {
  Boot, Unprovisioned, LocalCommandPending, ManualRevalidationPending,
  ExactOemQuery, ExternalStateTraffic, AmbiguousOemYield, ContradictoryTail,
  ConsensusTimeout, TransactionExhausted, EstimatedTimerDeadline,
  LearningStarted, SenderChanged, RadioUnavailable, RestoredUnverified
};
enum class PersistenceKind : std::uint8_t { SaveProvisioning, EraseProvisioning, SaveRememberedSpeed, SaveSpeedCapability };
enum class CoreEventKind : std::uint8_t {
  Diagnostic, RequestAccepted, RequestRefused, CandidateObserved,
  ConsensusReached, AuthorityChanged, TransactionFinished, OemPriority,
  RecoveryScheduled, StaleTxCallback, InvalidInternalEvent, WatchdogFired
};

struct TxRequest final {
  TxToken token;
  FrameBytes payload;
  TxReason reason;
  std::optional<TransactionId> transaction;
  std::optional<AttemptNumber> attempt;
  MonotonicMs issued_ms;
};

struct CoreEvent final {
  CoreEventKind kind;
  CoordinatorState state;
  std::optional<RefusalReason> refusal;
  std::optional<TransactionOutcome> outcome;
  std::optional<TxToken> token;
};

constexpr MonotonicMs kPostCommandAcceptStartMs = 400;
constexpr MonotonicMs kPostCommandAcceptEndMs = 1600;
constexpr MonotonicMs kDirectQueryAcceptStartMs = 300;
constexpr MonotonicMs kDirectQueryAcceptEndMs = 1100;
constexpr MonotonicMs kResponseTailEndMs = 2500;
constexpr MonotonicMs kMinIndependentCandidateGapMs = 60;
// Passive correlation reuses the field-validated post-command reply envelope.
// Repository captures place direct replies at roughly 417-648 ms and retain a
// 400-1600 ms post-command acceptance window. These bounds are defensive burst
// correlation for command-shaped values, not a protocol direction bit.
constexpr MonotonicMs kPassiveReplyAcceptStartMs = 400;
constexpr MonotonicMs kPassiveReplyAcceptEndMs = 1600;
// Live OEM callbacks within one three-frame press are about 102 ms apart, and
// the documented duplicate epoch spans 300 ms from its first frame. Requiring
// this much silence after the last first-burst frame prevents a held repeat
// train from becoming its own passive reply.
constexpr MonotonicMs kPassiveInterBurstSilenceMs = 300;
constexpr MonotonicMs kLearnSightingGapMs = 600;   // min separation between independent learn sightings
constexpr MonotonicMs kLearnWindowSpanMs = 60000;  // stale boundary: a same-sender frame this old restarts the candidate
constexpr std::uint8_t kLearnMinSightings = 3;     // independent sightings required before binding a fan
constexpr MonotonicMs kCommandRetryMinDelayMs = 1000;
constexpr MonotonicMs kInterFrameGapMs = 45;
constexpr MonotonicMs kOemHoldoffMs = 2000;
constexpr MonotonicMs kOemRecoveryQuietMs = 3000;
constexpr MonotonicMs kOemRecoveryMaxAgeMs = 30000;
constexpr MonotonicMs kTimerExpiryRecoveryMaxAgeMs = 5000;
constexpr MonotonicMs kTxLeaseStartWatchdogMs = 500;
constexpr MonotonicMs kTxBurstWatchdogMs = 1500;
constexpr std::uint8_t kRadioRecoveryAttempts = 2;

inline std::optional<MonotonicMs> elapsed_since(MonotonicMs now, MonotonicMs anchor) {
  if (now < anchor) return std::nullopt;
  return now - anchor;
}

constexpr MonotonicMs saturating_add(MonotonicMs value, MonotonicMs delta) {
  const auto maximum = std::numeric_limits<MonotonicMs>::max();
  return delta > maximum - value ? maximum : value + delta;
}

// Shared deterministic seed mixer for bounded timing jitter. Keeping the mix
// platform-free lets recovery and adapter heartbeat scheduling use identical
// seed semantics without a random engine or heap-backed state.
constexpr std::uint32_t deterministic_jitter_mix(
    std::uint32_t seed, std::uint32_t salt, MonotonicMs anchor_ms) {
  std::uint32_t mixed = seed ^ salt ^ static_cast<std::uint32_t>(anchor_ms) ^
                        static_cast<std::uint32_t>(anchor_ms >> 32U);
  mixed ^= mixed >> 16U;
  mixed *= 0x7FEB352DU;
  mixed ^= mixed >> 15U;
  return mixed;
}

}  // namespace quietcool
