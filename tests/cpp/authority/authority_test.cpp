#include "quietcool/core/authority_store.h"
#include "support/test.h"

#include <variant>

namespace quietcool {
namespace {

AcceptedObservation accepted(FanState state, EvidenceSource source,
                             MonotonicMs observed_ms) {
  return {state, SpeedCapability::Three, source,
          EvidenceConfidence::ExactBackedConsensus, observed_ms, 2,
          std::nullopt, std::nullopt, std::nullopt, false};
}

QC_TEST("authority", "query timer promotes fan state with unknown remaining time") {
  AuthorityStore authority;
  authority.promote(accepted(FanState::observed(0xD1).value(),
                             EvidenceSource::ManualQueryConsensus, 100), 100);
  const auto snapshot = authority.snapshot(100);
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(snapshot.state));
  QC_CHECK(std::holds_alternative<ProgrammedDurationAuthority>(snapshot.timer));
  QC_CHECK_EQ(std::get<ProgrammedDurationAuthority>(snapshot.timer).duration,
              Duration::Hours1);
}

QC_TEST("authority", "local matching timer gets conservative completion anchor") {
  AuthorityStore authority;
  auto value = accepted(FanState::observed(0xD1).value(),
                        EvidenceSource::PostCommandConsensus, 1200);
  value.transaction = TransactionId(2);
  value.attempt = AttemptNumber(1);
  value.command_completed_ms = 400;
  value.anchors_local_timer = true;
  authority.promote(value, 1200);
  const auto timer = std::get<LocallyAnchoredTimerAuthority>(
      authority.snapshot(1200).timer);
  QC_CHECK_EQ(timer.anchor_ms, 400U);
  QC_CHECK_EQ(timer.expiry_ms, 3600400U);
}

QC_TEST("authority", "estimated expiry invalidates and never confirms OFF") {
  AuthorityStore authority;
  auto value = accepted(FanState::observed(0xD1).value(),
                        EvidenceSource::PostCommandConsensus, 1200);
  value.transaction = TransactionId(2);
  value.attempt = AttemptNumber(1);
  value.command_completed_ms = 400;
  value.anchors_local_timer = true;
  authority.promote(value, 1200);
  const auto early = authority.timer_estimate_expired(3600399);
  QC_CHECK_EQ(early.status, TimerExpiryStatus::NotDue);
  QC_CHECK(!early.persistence.has_value());
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(
      authority.snapshot(3600399).state));
  const auto due = authority.timer_estimate_expired(3600400);
  QC_CHECK_EQ(due.status, TimerExpiryStatus::Due);
  QC_CHECK(!due.persistence.has_value());
  const auto snapshot = authority.snapshot(3600400);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(snapshot.state));
  QC_CHECK_EQ(std::get<UnknownStateAuthority>(snapshot.state).reason,
              AuthorityLossReason::EstimatedTimerDeadline);
}

QC_TEST("authority", "manual revalidation keeps previous only diagnostically") {
  AuthorityStore authority;
  authority.promote(accepted(FanState::observed(0xDF).value(),
                             EvidenceSource::BootQueryConsensus, 10), 10);
  const auto revision = authority.snapshot(10).revision;
  authority.begin_manual_revalidation(20);
  const auto snapshot = authority.snapshot(20);
  QC_CHECK(std::holds_alternative<RevalidatingStateAuthority>(snapshot.state));
  QC_CHECK(std::get<RevalidatingStateAuthority>(snapshot.state).previous.has_value());
  QC_CHECK(snapshot.revision > revision);
}

QC_TEST("authority", "restore creates diagnostic hint and no authority") {
  AuthorityStore authority;
  RestorableState restored;
  restored.remembered_speed = Speed::High;
  restored.observation_hint = RestoredObservationHint{0x3F, SpeedCapability::Three};
  authority.restore_hint(restored, 0);
  const auto snapshot = authority.snapshot(0);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(snapshot.state));
  QC_CHECK_EQ(std::get<UnknownStateAuthority>(snapshot.state).reason,
              AuthorityLossReason::RestoredUnverified);
  QC_CHECK_EQ(snapshot.remembered_speed.value(), Speed::High);
}

}  // namespace
}  // namespace quietcool
