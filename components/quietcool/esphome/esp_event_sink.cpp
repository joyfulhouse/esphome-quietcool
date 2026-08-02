#include "esp_event_sink.h"

#include <cmath>

#include "esphome/core/log.h"

#include <limits>

namespace esphome::quietcool {
namespace {

constexpr char kTag[] = "quietcool.core";
constexpr ::quietcool::MonotonicMs kFrequentEventLogIntervalMs = 1000;
constexpr ::quietcool::MonotonicMs kTimerPublishIntervalMs = 60000;

const char* event_name(::quietcool::CoreEventKind kind) {
  using Kind = ::quietcool::CoreEventKind;
  switch (kind) {
    case Kind::Diagnostic: return "diagnostic";
    case Kind::RequestAccepted: return "request_accepted";
    case Kind::RequestRefused: return "request_refused";
    case Kind::CandidateObserved: return "candidate_observed";
    case Kind::ConsensusReached: return "consensus_reached";
    case Kind::AuthorityChanged: return "authority_changed";
    case Kind::TransactionFinished: return "transaction_finished";
    case Kind::OemPriority: return "oem_priority";
    case Kind::RecoveryScheduled: return "recovery_scheduled";
    case Kind::StaleTxCallback: return "stale_tx_callback";
    case Kind::InvalidInternalEvent: return "invalid_internal_event";
    case Kind::WatchdogFired: return "watchdog_fired";
  }
  return "unknown";
}

const char* state_name(::quietcool::CoordinatorState state) {
  using State = ::quietcool::CoordinatorState;
  switch (state) {
    case State::Unprovisioned: return "unprovisioned";
    case State::Idle: return "idle";
    case State::CommandPending: return "command_pending";
    case State::CommandLeaseIssued: return "command_lease_issued";
    case State::CommandTransmitting: return "command_transmitting";
    case State::PostCommandListening: return "post_command_listening";
    case State::PostCommandTailWait: return "post_command_tail_wait";
    case State::FallbackQueryPending: return "fallback_query_pending";
    case State::FallbackQueryLeaseIssued: return "fallback_query_lease";
    case State::FallbackQueryTransmitting: return "fallback_query_tx";
    case State::FallbackResponseListening: return "fallback_response";
    case State::RetryDelay: return "retry_delay";
    case State::BootQueryPending: return "boot_query_pending";
    case State::BootQueryLeaseIssued: return "boot_query_lease";
    case State::BootQueryTransmitting: return "boot_query_tx";
    case State::BootResponseListening: return "boot_response";
    case State::ManualQueryPending: return "manual_query_pending";
    case State::ManualQueryLeaseIssued: return "manual_query_lease";
    case State::ManualQueryTransmitting: return "manual_query_tx";
    case State::ManualResponseListening: return "manual_response";
    case State::OemHoldoff: return "oem_holdoff";
    case State::RecoveryQuietWait: return "recovery_quiet";
    case State::RecoveryQueryPending: return "recovery_query_pending";
    case State::RecoveryQueryLeaseIssued: return "recovery_query_lease";
    case State::RecoveryQueryTransmitting: return "recovery_query_tx";
    case State::RecoveryResponseListening: return "recovery_response";
    case State::RecoveryRetryWait: return "recovery_retry";
    case State::ResponseTailQuarantine: return "response_tail";
    case State::LearningAwaitingFirst: return "learning_first";
    case State::LearningAwaitingSecond: return "learning_second";
    case State::RadioRecovery: return "radio_recovery";
  }
  return "unknown";
}

bool frequent(::quietcool::CoreEventKind kind) {
  using Kind = ::quietcool::CoreEventKind;
  return kind == Kind::Diagnostic || kind == Kind::CandidateObserved ||
         kind == Kind::StaleTxCallback;
}

const char* outcome_name(::quietcool::TransactionOutcome outcome) {
  using Outcome = ::quietcool::TransactionOutcome;
  switch (outcome) {
    case Outcome::Confirmed: return "confirmed";
    case Outcome::Exhausted: return "exhausted";
    case Outcome::Superseded: return "superseded";
    case Outcome::CancelledByExactOemQuery: return "cancelled_by_oem_query";
    case Outcome::CancelledByExternalState: return "cancelled_by_external_state";
    case Outcome::YieldedToPossibleOemCommand: return "yielded_to_oem";
    case Outcome::CancelledForLearning: return "cancelled_for_learning";
    case Outcome::RadioUnavailable: return "radio_unavailable";
  }
  return "unknown";
}

const char* refusal_name(::quietcool::RefusalReason reason) {
  using Reason = ::quietcool::RefusalReason;
  switch (reason) {
    case Reason::Unprovisioned: return "unprovisioned";
    case Reason::Busy: return "busy";
    case Reason::Holdoff: return "holdoff";
    case Reason::Learning: return "learning";
    case Reason::InvalidState: return "invalid_state";
    case Reason::IdExhausted: return "id_exhausted";
    case Reason::AmbiguousLearn: return "ambiguous_learn";
    case Reason::AlreadyProvisioned: return "already_provisioned";
  }
  return "unknown";
}

const char* evidence_name(::quietcool::EvidenceSource source) {
  using Source = ::quietcool::EvidenceSource;
  switch (source) {
    case Source::BootQueryConsensus: return "boot_query";
    case Source::ManualQueryConsensus: return "manual_query";
    case Source::RecoveryQueryConsensus: return "recovery_query";
    case Source::TimerExpiryRecoveryConsensus: return "timer_expiry_recovery";
    case Source::PostCommandConsensus: return "post_command";
    case Source::FallbackQueryConsensus: return "fallback_query";
    case Source::ExternalDiagnostic: return "external_diagnostic";
    case Source::RestoredHint: return "restored_hint";
  }
  return "unknown";
}

}  // namespace

bool EspHomeEventSink::should_log(const ::quietcool::CoreEvent& event) {
  const auto index = static_cast<std::size_t>(event.kind);
  if (!frequent(event.kind) || index >= kEventKindCount) return true;
  if (!logged_[index] ||
      now_ms_ - last_log_ms_[index] >= kFrequentEventLogIntervalMs) {
    logged_[index] = true;
    last_log_ms_[index] = now_ms_;
    return true;
  }
  return false;
}

void EspHomeEventSink::publish_command_status(const char* status) {
  if (command_status_ != nullptr) command_status_->publish_state(status);
  command_status_published_ = true;
}

void EspHomeEventSink::publish_authority(
    const ::quietcool::AuthoritySnapshot& authority,
    ::quietcool::MonotonicMs now_ms) {
  const bool authority_changed =
      !published_authority_revision_ ||
      authority.revision != *published_authority_revision_;
  const auto* local_timer =
      std::get_if<::quietcool::LocallyAnchoredTimerAuthority>(&authority.timer);
  const bool timer_publish_due =
      local_timer != nullptr &&
      (!timer_remaining_published_ ||
       now_ms - last_timer_publish_ms_ >= kTimerPublishIntervalMs);
  if (!authority_changed && !timer_publish_due) return;

  if (timer_publish_due && timer_remaining_ != nullptr) {
    const auto remaining_ms =
        now_ms < local_timer->expiry_ms ? local_timer->expiry_ms - now_ms : 0;
    timer_remaining_->publish_state(
        static_cast<float>(remaining_ms) / 60000.0F);
    last_timer_publish_ms_ = now_ms;
    timer_remaining_published_ = true;
  }
  if (!authority_changed) return;
  published_authority_revision_ = authority.revision;

  const auto* confirmed =
      std::get_if<::quietcool::ConfirmedStateAuthority>(&authority.state);
  const auto* no_timer =
      std::get_if<::quietcool::NoActiveTimerAuthority>(&authority.timer);
  const bool timer_program_known =
      !std::holds_alternative<::quietcool::UnknownTimerAuthority>(
          authority.timer);

  if (state_known_ != nullptr) state_known_->publish_state(confirmed != nullptr);
  if (timer_program_known_ != nullptr)
    timer_program_known_->publish_state(timer_program_known);
  if (timer_remaining_known_ != nullptr)
    timer_remaining_known_->publish_state(local_timer != nullptr);
  if (confirmed_off_ != nullptr)
    confirmed_off_->publish_state(confirmed != nullptr && no_timer != nullptr &&
                                  no_timer->confirmed_off);

  if (timer_remaining_ != nullptr) {
    if (local_timer != nullptr && !timer_publish_due) {
      const auto remaining_ms =
          now_ms < local_timer->expiry_ms ? local_timer->expiry_ms - now_ms : 0;
      timer_remaining_->publish_state(
          static_cast<float>(remaining_ms) / 60000.0F);
      last_timer_publish_ms_ = now_ms;
      timer_remaining_published_ = true;
    } else {
      if (local_timer == nullptr) {
        timer_remaining_->publish_state(
            std::numeric_limits<float>::quiet_NaN());
        timer_remaining_published_ = false;
      }
    }
  }
  if (evidence_source_ != nullptr) {
    evidence_source_->publish_state(
        confirmed != nullptr ? evidence_name(confirmed->source) : "unavailable");
  }
  if (!command_status_published_) publish_command_status("idle");
}

void EspHomeEventSink::publish_controller_healthy() {
  if (controller_fault_ != nullptr) controller_fault_->publish_state(false);
}

void EspHomeEventSink::publish_controller_failed() {
  if (controller_fault_ != nullptr) controller_fault_->publish_state(true);
  if (state_known_ != nullptr) state_known_->publish_state(false);
  if (timer_program_known_ != nullptr)
    timer_program_known_->publish_state(false);
  if (timer_remaining_known_ != nullptr)
    timer_remaining_known_->publish_state(false);
  if (confirmed_off_ != nullptr) confirmed_off_->publish_state(false);
  if (command_status_ != nullptr) command_status_->publish_state("unavailable");
  // Round 6 (codex): these two otherwise retained their last live values
  // forever — Home Assistant showing "55 minutes remaining" and
  // "post_command" beside a raised Controller Fault. NaN renders as
  // unavailable for a sensor.
  if (timer_remaining_ != nullptr) timer_remaining_->publish_state(NAN);
  if (evidence_source_ != nullptr) evidence_source_->publish_state("unavailable");
}

void EspHomeEventSink::on_core_event(const ::quietcool::CoreEvent& event) {
  using Kind = ::quietcool::CoreEventKind;
  if (event.kind == Kind::RequestAccepted) {
    publish_command_status("pending");
  } else if (event.kind == Kind::RequestRefused) {
    publish_command_status("refused");
  } else if (event.kind == Kind::TransactionFinished && event.outcome) {
    publish_command_status(outcome_name(*event.outcome));
  }

  if (!should_log(event)) return;
  const auto* kind = event_name(event.kind);
  const auto* state = state_name(event.state);
  if (event.kind == Kind::InvalidInternalEvent) {
    ESP_LOGE(kTag, "event=%s state=%s", kind, state);
  } else if (event.kind == Kind::RequestRefused) {
    // The reason is part of the user-visible surface: "already_provisioned"
    // (issue #16) is how an operator learns that re-pairing needs Forget first.
    ESP_LOGW(kTag, "event=%s state=%s reason=%s", kind, state,
             event.refusal ? refusal_name(*event.refusal) : "unknown");
  } else if (event.kind == Kind::WatchdogFired ||
             event.kind == Kind::StaleTxCallback) {
    ESP_LOGW(kTag, "event=%s state=%s", kind, state);
  } else if (event.kind == Kind::AuthorityChanged ||
             event.kind == Kind::TransactionFinished ||
             event.kind == Kind::OemPriority) {
    ESP_LOGI(kTag, "event=%s state=%s", kind, state);
  } else {
    ESP_LOGD(kTag, "event=%s state=%s", kind, state);
  }
}

}  // namespace esphome::quietcool
