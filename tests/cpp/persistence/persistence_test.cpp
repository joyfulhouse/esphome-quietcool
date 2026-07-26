#include "quietcool/core/confirmation_core.h"
#include "support/test.h"

#include <optional>
#include <limits>
#include <variant>

namespace quietcool {
namespace {

SenderId persistence_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

std::optional<TxRequest> persistence_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* request = std::get_if<RequestTxBurst>(&effects[index]))
      return request->request;
  return std::nullopt;
}

std::optional<PersistenceRequest> persistence_request(
    const CoreEffects& effects, PersistenceKind kind) {
  for (std::size_t index = 0; index < effects.size(); ++index) {
    const auto* request = std::get_if<RequestPersistenceEffect>(&effects[index]);
    if (request && request->request.kind == kind) return request->request;
  }
  return std::nullopt;
}

std::optional<AuthoritySnapshot> published_authority(
    const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* publish =
            std::get_if<PublishAuthorityEffect>(&effects[index]))
      return publish->authority;
  return std::nullopt;
}

std::size_t persistence_count(const CoreEffects& effects,
                              PersistenceKind kind) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < effects.size(); ++index) {
    const auto* request = std::get_if<RequestPersistenceEffect>(&effects[index]);
    count += request && request->request.kind == kind;
  }
  return count;
}

CoreEffects confirm_manual(ConfirmationCore& core, std::uint8_t raw_state,
                           MonotonicMs start_ms) {
  core.request_manual_refresh(start_ms);
  const auto query = persistence_tx(core.poll(start_ms));
  QC_CHECK(query.has_value());
  core.on_tx_started(query->token, start_ms);
  core.on_tx_complete(query->token, start_ms + 100);
  const FrameBytes response{{0xCB, 0x00, 0x47, 0x39, raw_state, raw_state}};
  core.on_frame(ByteView(response.bytes), start_ms + 300);
  return core.on_frame(ByteView(response.bytes), start_ms + 360);
}

ConfirmationCore restored_core(std::optional<Speed> remembered = std::nullopt) {
  ConfirmationCore core(CoreConfig{59});
  RestorableState restored;
  restored.sender = persistence_sender();
  restored.remembered_speed = remembered;
  core.restore(restored, 0);
  return core;
}

QC_TEST("persistence", "remembered speed is saved only when it changes") {
  auto core = restored_core();
  auto effects = confirm_manual(core, 0xDF, 0);
  auto save = persistence_request(effects, PersistenceKind::SaveRememberedSpeed);
  QC_CHECK(save.has_value());
  QC_CHECK_EQ(save->remembered_speed.value(), Speed::Low);
  QC_CHECK_EQ(persistence_count(effects, PersistenceKind::SaveRememberedSpeed),
              1U);

  core.poll(kResponseTailEndMs + 1);
  effects = confirm_manual(core, 0x9F, 3000);
  QC_CHECK_EQ(persistence_count(effects, PersistenceKind::SaveRememberedSpeed),
              0U);

  core.poll(3000 + kResponseTailEndMs + 1);
  effects = confirm_manual(core, 0xFF, 6000);
  save = persistence_request(effects, PersistenceKind::SaveRememberedSpeed);
  QC_CHECK(save.has_value());
  QC_CHECK_EQ(save->remembered_speed.value(), Speed::High);
}

QC_TEST("persistence", "restored remembered speed suppresses a redundant save") {
  auto core = restored_core(Speed::Low);
  const auto effects = confirm_manual(core, 0xDF, 0);
  QC_CHECK_EQ(persistence_count(effects, PersistenceKind::SaveRememberedSpeed),
              0U);
}

QC_TEST("persistence", "restore rejects incompatible schema and seed policy") {
  for (const bool invalid_version : {false, true}) {
    ConfirmationCore core(CoreConfig{59});
    RestorableState restored;
    restored.version = invalid_version ? 2 : 1;
    restored.seed_policy = invalid_version
        ? SeedPolicy::AllowCompiledSeed
        : static_cast<SeedPolicy>(0xFF);
    restored.sender = persistence_sender();
    restored.remembered_speed = Speed::High;
    restored.observation_hint =
        RestoredObservationHint{0x3F, SpeedCapability::Three};
    core.restore(restored, 10);
    const auto snapshot = core.snapshot(10);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::Unprovisioned);
    QC_CHECK(!snapshot.authority.remembered_speed.has_value());
    const auto& unknown =
        std::get<UnknownStateAuthority>(snapshot.authority.state);
    QC_CHECK_EQ(unknown.reason, AuthorityLossReason::Unprovisioned);
    QC_CHECK(!unknown.restored_hint.has_value());
  }
}

QC_TEST("persistence", "restore validates typed fields and accepts suppression") {
  {
    ConfirmationCore core(CoreConfig{59});
    RestorableState restored;
    restored.seed_policy = SeedPolicy::SuppressCompiledSeed;
    restored.sender = persistence_sender();
    restored.remembered_speed = Speed::Low;
    restored.observation_hint =
        RestoredObservationHint{0x1F, SpeedCapability::Three};
    core.restore(restored, 10);
    const auto snapshot = core.snapshot(10);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::Idle);
    QC_CHECK_EQ(snapshot.authority.remembered_speed.value(), Speed::Low);
    QC_CHECK(std::get<UnknownStateAuthority>(snapshot.authority.state)
                 .restored_hint.has_value());
  }
  for (const std::uint8_t invalid_case : {0U, 1U, 2U}) {
    ConfirmationCore core(CoreConfig{59});
    RestorableState restored;
    restored.sender = persistence_sender();
    if (invalid_case == 0)
      restored.remembered_speed = static_cast<Speed>(0);
    else if (invalid_case == 1)
      restored.observation_hint =
          RestoredObservationHint{0x13, SpeedCapability::Three};
    else
      restored.observation_hint =
          RestoredObservationHint{0x3F, SpeedCapability::Two};
    core.restore(restored, 10);
    QC_CHECK_EQ(core.snapshot(10).state, CoordinatorState::Unprovisioned);
  }
}

QC_TEST("persistence", "restore clears volatile work without resurrecting RF") {
  auto core = restored_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  QC_CHECK(persistence_tx(core.poll(0)).has_value());

  RestorableState restored;
  restored.sender = persistence_sender();
  const auto effects = core.restore(restored, 1);
  const auto snapshot = core.snapshot(1);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::Idle);
  QC_CHECK(!snapshot.transaction.has_value());
  QC_CHECK(!snapshot.live_tx.has_value());
  QC_CHECK(!snapshot.deferred_command.has_value());
  QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::Inactive);
  QC_CHECK(!snapshot.learning.active);
  QC_CHECK_EQ(persistence_count(effects, PersistenceKind::SaveProvisioning),
              0U);
}

// Issue #31: the capability persisted beside remembered_speed must come back
// out of restore() — seeded into the authority snapshot AND published, so the
// fan entity can seed its speed count before Home Assistant can command it.
QC_TEST("persistence", "restore seeds and publishes the persisted capability") {
  ConfirmationCore core(CoreConfig{59});
  RestorableState restored;
  restored.sender = persistence_sender();
  restored.speed_capability = SpeedCapability::Two;
  const auto effects = core.restore(restored, 0);
  const auto published = published_authority(effects);
  QC_CHECK(published.has_value());
  QC_CHECK_EQ(published->speed_capability.value(), SpeedCapability::Two);
  QC_CHECK_EQ(core.snapshot(0).authority.speed_capability.value(),
              SpeedCapability::Two);
}

QC_TEST("persistence", "confirmed capability is saved only when it changes") {
  auto core = restored_core();
  // 0x9F: capability Two, LOW, continuous. First confirmation persists it.
  auto effects = confirm_manual(core, 0x9F, 0);
  auto save = persistence_request(effects, PersistenceKind::SaveSpeedCapability);
  QC_CHECK(save.has_value());
  QC_CHECK_EQ(save->speed_capability.value(), SpeedCapability::Two);
  QC_CHECK_EQ(persistence_count(effects, PersistenceKind::SaveSpeedCapability),
              1U);

  // Same capability again: no NVS write per promote.
  core.poll(kResponseTailEndMs + 1);
  effects = confirm_manual(core, 0x9F, 3000);
  QC_CHECK_EQ(persistence_count(effects, PersistenceKind::SaveSpeedCapability),
              0U);

  // 0xDF: capability Three. A genuine change persists again.
  core.poll(3000 + kResponseTailEndMs + 1);
  effects = confirm_manual(core, 0xDF, 6000);
  save = persistence_request(effects, PersistenceKind::SaveSpeedCapability);
  QC_CHECK(save.has_value());
  QC_CHECK_EQ(save->speed_capability.value(), SpeedCapability::Three);
}

QC_TEST("persistence", "restored capability suppresses a redundant save") {
  ConfirmationCore core(CoreConfig{59});
  RestorableState restored;
  restored.sender = persistence_sender();
  restored.speed_capability = SpeedCapability::Two;
  core.restore(restored, 0);
  const auto effects = confirm_manual(core, 0x9F, 0);
  QC_CHECK_EQ(persistence_count(effects, PersistenceKind::SaveSpeedCapability),
              0U);
}

QC_TEST("persistence", "restore rejects an unconfirmed or corrupt capability") {
  for (const auto capability :
       {SpeedCapability::Unknown, static_cast<SpeedCapability>(7)}) {
    ConfirmationCore core(CoreConfig{59});
    RestorableState restored;
    restored.sender = persistence_sender();
    restored.speed_capability = capability;
    core.restore(restored, 10);
    const auto snapshot = core.snapshot(10);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::Unprovisioned);
    QC_CHECK(!snapshot.authority.speed_capability.has_value());
  }
}

// Issue #31 option 3: a command whose FanState byte was frozen before the
// fan's capability was known is re-aimed at frame-build time. MED (0xAF) on a
// 2-speed fan stops it (issue #30); the positional rule maps the unsupported
// speed to HIGH, the top of the band.
QC_TEST("persistence", "stale Medium command retargets to High under capability Two") {
  // Control: with no confirmed capability the Medium byte transmits as-is —
  // this is the leg a broken re-aim substitution would leave green.
  {
    auto core = restored_core();
    core.request_state(FanState::command(Speed::Medium, Duration::Continuous),
                       0);
    const auto tx = persistence_tx(core.poll(0));
    QC_CHECK(tx.has_value());
    QC_CHECK_EQ(tx->payload.bytes[4], 0xAF);
  }
  ConfirmationCore core(CoreConfig{59});
  RestorableState restored;
  restored.sender = persistence_sender();
  restored.speed_capability = SpeedCapability::Two;
  core.restore(restored, 0);
  core.request_state(FanState::command(Speed::Medium, Duration::Continuous), 0);
  const auto tx = persistence_tx(core.poll(0));
  QC_CHECK(tx.has_value());
  QC_CHECK_EQ(tx->payload.bytes[4], 0xBF);
  // The corrected speed is also what confirmation will compare against: a
  // requested_ left at Medium would misread the fan's High report as an OEM
  // override and yield.
  const auto transaction = core.snapshot(0).transaction;
  QC_CHECK(transaction.has_value());
  QC_CHECK_EQ(transaction->requested.canonical_byte(), 0x3F);
}

// The capability-One band: a 1-speed fan supports only HIGH, so both LOW and
// MED are out-of-band and re-aim to the top. Pins the exact band predicate —
// a filter admitting "anything but Low" (or any other near-miss) fails here.
QC_TEST("persistence", "capability One re-aims Low and Medium to High") {
  for (const auto speed : {Speed::Low, Speed::Medium}) {
    ConfirmationCore core(CoreConfig{59});
    RestorableState restored;
    restored.sender = persistence_sender();
    restored.speed_capability = SpeedCapability::One;
    core.restore(restored, 0);
    core.request_state(FanState::command(speed, Duration::Continuous), 0);
    const auto tx = persistence_tx(core.poll(0));
    QC_CHECK(tx.has_value());
    QC_CHECK_EQ(tx->payload.bytes[4], 0xBF);
  }
}

// The filter must be surgical: in-band speeds pass through byte-identical.
QC_TEST("persistence", "supported speeds pass the capability filter untouched") {
  ConfirmationCore core(CoreConfig{59});
  RestorableState restored;
  restored.sender = persistence_sender();
  restored.speed_capability = SpeedCapability::Two;
  core.restore(restored, 0);
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto tx = persistence_tx(core.poll(0));
  QC_CHECK(tx.has_value());
  QC_CHECK_EQ(tx->payload.bytes[4], 0x9F);
}

// Every attempt's frame passes the re-aim choke point, not just the first:
// reaim_off_to can rewrite an OFF transaction's outbound to the speed of a
// mid-transaction ECHO (our own stale Medium command heard back), which
// re-introduces an out-of-band nibble after attempt 1 was already corrected.
// The retry must go out re-aimed. A first-attempt-only re-aim fails here.
QC_TEST("persistence", "off retry is re-aimed after an echo reintroduces Medium") {
  ConfirmationCore core(CoreConfig{59});
  RestorableState restored;
  restored.sender = persistence_sender();
  restored.speed_capability = SpeedCapability::Two;
  core.restore(restored, 0);

  core.request_state(FanState::command(Speed::High, Duration::Off), 0);
  const auto first = persistence_tx(core.poll(0));
  QC_CHECK(first.has_value());
  QC_CHECK_EQ(first->payload.bytes[4], 0xB0);  // HIGH|Off, in-band
  core.on_tx_started(first->token, 0);
  core.on_tx_complete(first->token, 400);

  // Consensus on a marker frame carrying ON Medium (0xAF): decide_off retries
  // without promotion and reaim_off_to rewrites outbound to Medium|Off (0xA0).
  const FrameBytes echo{{0xCB, 0x00, 0x47, 0x39, 0xAF, 0xAF}};
  core.on_frame(ByteView(echo.bytes), 1300);
  core.on_frame(ByteView(echo.bytes), 1300 + kMinIndependentCandidateGapMs);

  core.poll(400 + kResponseTailEndMs + 1);  // tail expiry -> retry pending
  const auto retry = persistence_tx(core.poll(400 + kResponseTailEndMs + 2));
  QC_CHECK(retry.has_value());
  QC_CHECK_EQ(retry->attempt.value().value(), 2U);
  QC_CHECK_EQ(retry->payload.bytes[4], 0xB0);  // re-aimed back in-band
}

// Issue #31: Forget destroys the fan BINDING, so the RAM sticky goes with the
// NVS record — the old fan's capability must not re-aim a later fan's commands.
QC_TEST("persistence", "forget clears the sticky capability in RAM") {
  ConfirmationCore core(CoreConfig{59});
  RestorableState restored;
  restored.sender = persistence_sender();
  restored.speed_capability = SpeedCapability::Two;
  core.restore(restored, 0);
  QC_CHECK_EQ(core.snapshot(0).authority.speed_capability.value(),
              SpeedCapability::Two);
  const auto effects = core.request_forget(1);
  QC_CHECK_EQ(persistence_count(effects, PersistenceKind::EraseProvisioning),
              1U);
  QC_CHECK(!core.snapshot(1).authority.speed_capability.has_value());
}

QC_TEST("persistence", "transaction IDs exhaust without wrapping or reuse") {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  MonotonicIdAllocator<TransactionId> ids(maximum);
  const auto last = ids.allocate();
  QC_CHECK(last.has_value());
  QC_CHECK_EQ(last->value(), maximum);
  QC_CHECK(ids.exhausted());
  QC_CHECK(!ids.allocate().has_value());
  QC_CHECK(!ids.allocate().has_value());
}

}  // namespace
}  // namespace quietcool
