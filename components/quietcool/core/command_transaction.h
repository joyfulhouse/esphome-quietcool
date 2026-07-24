#pragma once

#include "fan_state.h"

namespace quietcool {

struct PriorAuthoritySnapshot final {
  FanState state;
  EvidenceSource source;
  MonotonicMs observed_ms;
  bool valid_at_capture;
  std::uint64_t revision;
  bool operator==(const PriorAuthoritySnapshot& other) const {
    return state == other.state && source == other.source &&
           observed_ms == other.observed_ms &&
           valid_at_capture == other.valid_at_capture && revision == other.revision;
  }
};

enum class JoinDecision : std::uint8_t { SemanticDuplicate, Different };
enum class BudgetError : std::uint8_t { Exhausted, Terminal };

struct TransactionSnapshot final {
  TransactionId id;
  FanState requested;
  FanState outbound_command;
  std::uint8_t attempt_limit;
  std::uint8_t attempts_started;
  RefireCount remaining_refires;
  std::optional<PriorAuthoritySnapshot> prior_authority;
  std::optional<TransactionOutcome> outcome;
  bool fallback_used_for_attempt;
};

class CommandTransaction final {
 public:
  static CommandTransaction begin(
      TransactionId id, FanState requested,
      std::optional<PriorAuthoritySnapshot> prior_authority);
  JoinDecision compare_request(FanState requested) const;
  Result<AttemptNumber, BudgetError> note_command_burst_started();
  bool may_emit_another_command() const;
  RefireCount remaining_refires() const;
  void reaim_off_to(Speed reported_speed);
  void finish(TransactionOutcome outcome);
  bool mark_fallback_used();
  TransactionSnapshot snapshot() const;
 private:
  CommandTransaction(TransactionId id, FanState requested,
                     std::optional<PriorAuthoritySnapshot> prior, std::uint8_t limit)
      : id_(id), requested_(requested), outbound_(requested), limit_(limit),
        prior_(std::move(prior)) {}
  TransactionId id_;
  FanState requested_;
  FanState outbound_;
  std::uint8_t limit_;
  std::uint8_t started_{0};
  std::optional<PriorAuthoritySnapshot> prior_;
  std::optional<TransactionOutcome> outcome_;
  bool fallback_used_{false};
};

}  // namespace quietcool
