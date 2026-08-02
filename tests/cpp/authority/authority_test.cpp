#include "quietcool/core/authority_store.h"
#include "support/test.h"

#include <variant>

namespace quietcool {
namespace {

AcceptedObservation accepted(FanState state, EvidenceSource source,
                             MonotonicMs observed_ms) {
  return {state, SpeedCapability::Three, CapabilityEvidence::Unambiguous,
          source, EvidenceConfidence::ExactBackedConsensus, observed_ms, 2,
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

QC_TEST("authority", "a programmed duration carries its upper-bound deadline") {
  // Issue #35: the display half of the programmed-duration gap. The start
  // time is unknown, but a Hours1 program observed at t=100 cannot still be
  // running at t=100+1h — the estimated deadline fires there, taking the
  // SAME invalidate-then-recover path the anchored variant takes, which is
  // what clears the stale "1 hour, fan ON" display.
  AuthorityStore authority;
  authority.promote(accepted(FanState::observed(0xD1).value(),
                             EvidenceSource::ManualQueryConsensus, 100), 100);
  QC_CHECK(authority.timer_deadline().has_value());
  QC_CHECK_EQ(*authority.timer_deadline(), 3600100U);
  QC_CHECK_EQ(authority.timer_estimate_expired(3600099).status,
              TimerExpiryStatus::NotDue);
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(
      authority.snapshot(3600099).state));
  const auto due = authority.timer_estimate_expired(3600100);
  QC_CHECK_EQ(due.status, TimerExpiryStatus::Due);
  const auto snapshot = authority.snapshot(3600100);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(snapshot.state));
  QC_CHECK_EQ(std::get<UnknownStateAuthority>(snapshot.state).reason,
              AuthorityLossReason::EstimatedTimerDeadline);
}

QC_TEST("authority", "the programmed deadline bound is exact for every timed duration") {
  // The timed enumerators' values ARE their hour counts; a mapping typo here
  // fires the deadline hours early (presuming a running fan stopped) or
  // hours late (the stale display this exists to clear).
  const struct { std::uint8_t byte; MonotonicMs run; } cases[] = {
      {0xD1, 3600000ULL},  {0xD2, 7200000ULL},  {0xD4, 14400000ULL},
      {0xD8, 28800000ULL}, {0xDC, 43200000ULL}};
  for (const auto& c : cases) {
    AuthorityStore authority;
    authority.promote(accepted(FanState::observed(c.byte).value(),
                               EvidenceSource::ManualQueryConsensus, 500), 500);
    QC_CHECK(authority.timer_deadline().has_value());
    QC_CHECK_EQ(*authority.timer_deadline(), 500U + c.run);
  }
}

QC_TEST("authority", "a continuous observation carries no deadline") {
  // Continuous routes to NoActiveTimer in promote(); no deadline may fire —
  // an estimated deadline on a run-until-stopped fan would invalidate a
  // perfectly current display forever.
  AuthorityStore authority;
  authority.promote(accepted(FanState::observed(0xDF).value(),
                             EvidenceSource::ManualQueryConsensus, 100), 100);
  QC_CHECK(!authority.timer_deadline().has_value());
  QC_CHECK_EQ(authority.timer_estimate_expired(90000000).status,
              TimerExpiryStatus::NotDue);
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

// Issue #31: capability describes the bound fan, not authority freshness, so
// it must survive every invalidation and only move on confirmed evidence.
QC_TEST("authority", "confirmed capability is sticky across invalidation") {
  AuthorityStore authority;
  AcceptedObservation value{FanState::observed(0x9F).value(),
                            SpeedCapability::Two,
                            CapabilityEvidence::Unambiguous,
                            EvidenceSource::ManualQueryConsensus,
                            EvidenceConfidence::ExactBackedConsensus,
                            100,
                            2,
                            std::nullopt,
                            std::nullopt,
                            std::nullopt,
                            false};
  authority.promote(value, 100);
  QC_CHECK_EQ(authority.snapshot(100).speed_capability.value(),
              SpeedCapability::Two);

  authority.invalidate(AuthorityLossReason::ExternalStateTraffic, 200);
  QC_CHECK_EQ(authority.snapshot(200).speed_capability.value(),
              SpeedCapability::Two);

  // A report carrying no capability must not clobber the sticky value.
  value.capability = SpeedCapability::Unknown;
  authority.promote(value, 300);
  QC_CHECK_EQ(authority.snapshot(300).speed_capability.value(),
              SpeedCapability::Two);

  // restore_hint reseeds wholesale: no persisted capability means none.
  RestorableState restored;
  authority.restore_hint(restored, 400);
  QC_CHECK(!authority.snapshot(400).speed_capability.has_value());
}

QC_TEST("authority", "restore seeds the sticky capability") {
  AuthorityStore authority;
  RestorableState restored;
  restored.speed_capability = SpeedCapability::Two;
  authority.restore_hint(restored, 0);
  QC_CHECK_EQ(authority.snapshot(0).speed_capability.value(),
              SpeedCapability::Two);
  // And it survives the immediate boot-time invalidations too.
  authority.invalidate(AuthorityLossReason::Unprovisioned, 1);
  QC_CHECK_EQ(authority.snapshot(1).speed_capability.value(),
              SpeedCapability::Two);
}

// The ONLY sanctioned clear: the fan binding itself changed (Forget, or
// learning a different fan). Freshness invalidations must never do this.
QC_TEST("authority", "clear_confirmed_capability drops the sticky value") {
  AuthorityStore authority;
  auto value = accepted(FanState::observed(0x9F).value(),
                        EvidenceSource::ManualQueryConsensus, 100);
  value.capability = SpeedCapability::Two;
  authority.promote(value, 100);
  QC_CHECK_EQ(authority.snapshot(100).speed_capability.value(),
              SpeedCapability::Two);
  authority.clear_confirmed_capability();
  QC_CHECK(!authority.snapshot(200).speed_capability.has_value());
  // And a later confirmed report re-establishes it from evidence, not memory.
  authority.promote(accepted(FanState::observed(0xDF).value(),
                             EvidenceSource::ManualQueryConsensus, 300), 300);
  QC_CHECK_EQ(authority.snapshot(300).speed_capability.value(),
              SpeedCapability::Three);
}

// Issue #31 review: capability evidence that could be our own echo is admitted
// only into an EMPTY slot. That is the whole reason a 2-speed fan — whose
// confirming report is byte-identical to the command by construction — can
// still teach its band, while an echo can never demote a fan whose capability
// is already known from a frame that could not have been ours.
QC_TEST("authority", "echo-ranked capability fills an empty slot but never overwrites") {
  {
    AuthorityStore authority;
    auto value = accepted(FanState::observed(0x9F).value(),
                          EvidenceSource::PostCommandConsensus, 100);
    value.capability = SpeedCapability::Two;
    value.capability_evidence = CapabilityEvidence::PossiblyOwnEcho;
    authority.promote(value, 100);
    QC_CHECK_EQ(authority.snapshot(100).speed_capability.value(),
                SpeedCapability::Two);
  }
  {
    AuthorityStore authority;
    // Known Three from an unambiguous frame; an echo-ranked Two must bounce.
    authority.promote(accepted(FanState::observed(0xDF).value(),
                               EvidenceSource::BootQueryConsensus, 100), 100);
    auto echo = accepted(FanState::observed(0x9F).value(),
                         EvidenceSource::PostCommandConsensus, 200);
    echo.capability = SpeedCapability::Two;
    echo.capability_evidence = CapabilityEvidence::PossiblyOwnEcho;
    authority.promote(echo, 200);
    QC_CHECK_EQ(authority.snapshot(200).speed_capability.value(),
                SpeedCapability::Three);
    // ...and the mis-learned case self-heals: an unambiguous frame always wins.
    auto genuine = accepted(FanState::observed(0x9F).value(),
                            EvidenceSource::ManualQueryConsensus, 300);
    genuine.capability = SpeedCapability::Two;
    authority.promote(genuine, 300);
    QC_CHECK_EQ(authority.snapshot(300).speed_capability.value(),
                SpeedCapability::Two);
  }
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
