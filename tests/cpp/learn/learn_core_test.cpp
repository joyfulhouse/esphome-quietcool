#include "quietcool/core/confirmation_core.h"
#include "support/core_test_builder.h"
#include "support/test.h"

#include <array>
#include <cstddef>

// Core-level guards for the Learn path.
//
// Issue #6: a second distinct fan heard during Learn must refuse and leave NVS
// untouched. The assertion that actually protects the binding is that NO
// SaveProvisioning effect is emitted on the ambiguous path.
//
// Issue #16: while a sender is bound — learned or seeded — a Learn request is
// refused outright (AlreadyProvisioned). Re-learning is deliberately two
// steps: Forget, then Learn. The assertions that protect the binding are that
// a refused Learn emits no persistence effect of any kind, opens no window,
// and leaves the core commanding the same fan.

namespace quietcool {
namespace {

constexpr std::array<std::uint8_t, 6> command(std::uint8_t id,
                                              std::uint8_t state) {
  return {0xCB, 0x00, 0x47, id, state, state};
}

std::size_t save_provisioning_count(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < effects.size(); ++index) {
    const auto* request = std::get_if<RequestPersistenceEffect>(&effects[index]);
    count += request &&
             request->request.kind == PersistenceKind::SaveProvisioning;
  }
  return count;
}

std::size_t persistence_count(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < effects.size(); ++index)
    count += std::holds_alternative<RequestPersistenceEffect>(effects[index]);
  return count;
}

std::optional<RefusalReason> refusal_reason(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* refused = std::get_if<RefusedInput>(&effects[index]))
      return refused->reason;
  return std::nullopt;
}

SenderId bound_sender() { return SenderId::from_be_u32(0xCB004739U).value(); }

ConfirmationCore provisioned_core() {
  ConfirmationCore core(CoreConfig{23});
  RestorableState restored;
  restored.sender = bound_sender();
  core.restore(restored, 0);
  return core;
}

std::optional<TxRequest> tx_request(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* burst = std::get_if<RequestTxBurst>(&effects[index]))
      return burst->request;
  return std::nullopt;
}

// Drives an unprovisioned core through a full, legitimate Learn of fan `id`:
// window open plus three independent sightings (issue #6 evidence bar).
void learn_fan(ConfirmationCore& core, std::uint8_t id, MonotonicMs base_ms) {
  core.request_learn(LearnMode::Manual, base_ms);
  const auto frame = command(id, 0x9F);
  core.on_frame(ByteView(frame), base_ms + 1);
  core.on_frame(ByteView(frame), base_ms + 602);
  core.on_frame(ByteView(frame), base_ms + 1203);
}

QC_TEST("learn", "ambiguous relearn keeps the bound fan and writes no NVS") {
  // Since issue #16 a provisioned unit cannot reach a learning state through
  // the public API (request_learn refuses). The state is force-built here as
  // defence in depth: if a future change ever reopens a learn window while a
  // sender is bound, the two-fan ambiguity latch must still keep the binding.
  auto core = ConfirmationCoreTestBuilder::make_provisioned_learning(0);
  QC_CHECK_EQ(core.snapshot(0).state, CoordinatorState::LearningAwaitingFirst);

  const auto first = command(0x39, 0x9F);     // one fan on the air
  const auto intruder = command(0x40, 0xAF);  // a second, distinct fan
  core.on_frame(ByteView(first), 1);
  const auto effects = core.on_frame(ByteView(intruder), 700);

  QC_CHECK_EQ(save_provisioning_count(effects), std::size_t{0});
  QC_CHECK(refusal_reason(effects) == RefusalReason::AmbiguousLearn);
  // sender_ was left untouched, so a bound fan survives -> Idle (not Learned to
  // the intruder, not Unprovisioned).
  QC_CHECK_EQ(core.snapshot(700).state, CoordinatorState::Idle);
}

QC_TEST("learn", "ambiguous first-time learn binds nothing") {
  ConfirmationCore core(CoreConfig{23});  // unprovisioned
  core.request_learn(LearnMode::Manual, 0);
  core.on_frame(ByteView(command(0x39, 0x9F)), 1);
  const auto effects = core.on_frame(ByteView(command(0x40, 0xAF)), 700);

  QC_CHECK_EQ(save_provisioning_count(effects), std::size_t{0});
  QC_CHECK(refusal_reason(effects) == RefusalReason::AmbiguousLearn);
  // Nothing was ever bound, so the core falls back to Unprovisioned.
  QC_CHECK_EQ(core.snapshot(700).state, CoordinatorState::Unprovisioned);
}

QC_TEST("learn", "learn is refused while a restored sender is bound") {
  // restore() is also how a compiled-in seed reaches the core (the adapter
  // folds the seed into RestorableState), so this covers the seeded unit.
  auto core = provisioned_core();
  const auto effects = core.request_learn(LearnMode::Manual, 10);

  QC_CHECK(refusal_reason(effects) == RefusalReason::AlreadyProvisioned);
  // No learn window opened, nothing persisted, nothing else emitted.
  QC_CHECK_EQ(effects.size(), std::size_t{1});
  QC_CHECK_EQ(persistence_count(effects), std::size_t{0});
  const auto snapshot = core.snapshot(10);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::Idle);
  QC_CHECK(!snapshot.learning.active);

  // Frames after the refusal are ordinary traffic, not learn candidates: a
  // burst from a foreign fan cannot re-bind or move the core into learning.
  core.on_frame(ByteView(command(0x40, 0xAF)), 20);
  QC_CHECK_EQ(core.snapshot(20).state, CoordinatorState::Idle);
  QC_CHECK(!core.snapshot(20).learning.active);

  // The binding still commands the fan: a state request is accepted, which is
  // only possible with sender_ intact.
  ConfirmationCore fresh = provisioned_core();
  fresh.request_learn(LearnMode::Manual, 0);
  const auto accepted =
      fresh.request_state(FanState::command(Speed::Low, Duration::Continuous), 1);
  QC_CHECK(!refusal_reason(accepted).has_value());
  QC_CHECK_EQ(fresh.snapshot(1).state, CoordinatorState::CommandPending);
}

QC_TEST("learn", "learn is refused after a sender was learned over the air") {
  ConfirmationCore core(CoreConfig{23});
  learn_fan(core, 0x41, 0);
  QC_CHECK_EQ(core.snapshot(1203).state, CoordinatorState::Idle);

  const auto effects = core.request_learn(LearnMode::Manual, 2000);
  QC_CHECK(refusal_reason(effects) == RefusalReason::AlreadyProvisioned);
  QC_CHECK_EQ(persistence_count(effects), std::size_t{0});
  QC_CHECK_EQ(core.snapshot(2000).state, CoordinatorState::Idle);
  QC_CHECK(!core.snapshot(2000).learning.active);
}

QC_TEST("learn", "learn proceeds on an unprovisioned core") {
  ConfirmationCore core(CoreConfig{23});
  const auto effects = core.request_learn(LearnMode::Manual, 5);
  QC_CHECK(!refusal_reason(effects).has_value());
  const auto snapshot = core.snapshot(5);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::LearningAwaitingFirst);
  QC_CHECK(snapshot.learning.active);
}

// The refusal contract is "touch NOTHING", and the cheap way to violate it is
// to run one of handle_learn's teardown lines (window_.reset(),
// recovery_.cancel(), authority_.invalidate(), consensus_.reset()) before the
// sender_ gate instead of after. The four tests below pin the contract from
// non-idle trajectories where each of those mutations is observable:
// a half-collected confirmation window, confirmed authority, and both
// scheduler-driven recovery wait states (whose poll path only advances when
// the scheduler still holds the due event — cancelling strands them forever).

QC_TEST("learn", "refused learn leaves an in-flight confirmation window intact") {
  auto core = provisioned_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto cmd = tx_request(core.poll(0));
  QC_CHECK(cmd.has_value());
  core.on_tx_started(cmd->token, 0);
  core.on_tx_complete(cmd->token, 400);
  const auto response = command(0x39, 0xDF);
  core.on_frame(ByteView(response), 1105);  // first independent candidate

  const auto effects = core.request_learn(LearnMode::Manual, 1120);
  QC_CHECK(refusal_reason(effects) == RefusalReason::AlreadyProvisioned);
  const auto during = core.snapshot(1120);
  QC_CHECK_EQ(during.state, CoordinatorState::PostCommandListening);
  QC_CHECK(during.transaction.has_value());
  QC_CHECK(!during.learning.active);

  // The candidate heard before the refused Learn must still count: the second
  // sighting completes consensus exactly as if Learn had never been pressed.
  core.on_frame(ByteView(response), 1105 + kMinIndependentCandidateGapMs);
  const auto after = core.snapshot(1105 + kMinIndependentCandidateGapMs);
  QC_CHECK(after.last_transaction_outcome.has_value());
  QC_CHECK(after.last_transaction_outcome == TransactionOutcome::Confirmed);
}

QC_TEST("learn", "refused learn does not invalidate confirmed authority") {
  auto core = provisioned_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto cmd = tx_request(core.poll(0));
  QC_CHECK(cmd.has_value());
  core.on_tx_started(cmd->token, 0);
  core.on_tx_complete(cmd->token, 400);
  const auto response = command(0x39, 0xDF);
  core.on_frame(ByteView(response), 1105);
  core.on_frame(ByteView(response), 1105 + kMinIndependentCandidateGapMs);
  core.poll(2901);  // tail expires -> Idle with confirmed authority
  const auto before = core.snapshot(2950);
  QC_CHECK_EQ(before.state, CoordinatorState::Idle);
  const auto* confirmed_before =
      std::get_if<ConfirmedStateAuthority>(&before.authority.state);
  QC_CHECK(confirmed_before != nullptr);

  const auto effects = core.request_learn(LearnMode::Manual, 3000);
  QC_CHECK(refusal_reason(effects) == RefusalReason::AlreadyProvisioned);
  const auto after = core.snapshot(3000);
  const auto* confirmed_after =
      std::get_if<ConfirmedStateAuthority>(&after.authority.state);
  QC_CHECK(confirmed_after != nullptr);
  if (confirmed_before != nullptr && confirmed_after != nullptr) {
    QC_CHECK_EQ(confirmed_after->revision, confirmed_before->revision);
    QC_CHECK(confirmed_after->state.speed() == confirmed_before->state.speed());
  }
  QC_CHECK_EQ(after.authority.revision, before.authority.revision);
}

QC_TEST("learn", "refused learn leaves a recovery quiet-wait cycle intact") {
  auto core = provisioned_core();
  const auto oem = FrameCodec::encode_query(bound_sender());
  core.on_frame(ByteView(oem.bytes), 0);  // exact OEM query -> OemHoldoff
  core.poll(kOemHoldoffMs);
  QC_CHECK_EQ(core.snapshot(kOemHoldoffMs).state,
              CoordinatorState::RecoveryQuietWait);
  const auto before = core.snapshot(kOemHoldoffMs).recovery;

  const auto effects = core.request_learn(LearnMode::Manual, kOemHoldoffMs + 50);
  QC_CHECK(refusal_reason(effects) == RefusalReason::AlreadyProvisioned);
  const auto after = core.snapshot(kOemHoldoffMs + 50);
  QC_CHECK_EQ(after.state, CoordinatorState::RecoveryQuietWait);
  QC_CHECK(after.recovery.cause.has_value());
  QC_CHECK_EQ(after.recovery.phase, before.phase);
  QC_CHECK_EQ(after.recovery.due_ms, before.due_ms);
  QC_CHECK_EQ(after.recovery.expires_ms, before.expires_ms);

  // The scheduled recovery query must still fire at its due time.
  core.poll(kOemRecoveryQuietMs);
  const auto query = tx_request(core.poll(kOemRecoveryQuietMs));
  QC_CHECK(query.has_value());
  if (query)
    QC_CHECK(query->reason == TxReason::RecoveryQueryInitial);
}

QC_TEST("learn", "refused learn leaves a recovery retry-wait cycle intact") {
  auto core = provisioned_core();
  const auto oem = FrameCodec::encode_query(bound_sender());
  core.on_frame(ByteView(oem.bytes), 0);
  core.poll(kOemHoldoffMs);
  core.poll(kOemRecoveryQuietMs);
  const auto q1 = tx_request(core.poll(kOemRecoveryQuietMs));
  QC_CHECK(q1.has_value());
  core.on_tx_started(q1->token, kOemRecoveryQuietMs);
  core.on_tx_complete(q1->token, kOemRecoveryQuietMs + 100);
  core.poll(kOemRecoveryQuietMs + 1300);  // miss query 1 -> response tail
  core.poll(kOemRecoveryQuietMs + 2600);  // tail expires -> RecoveryRetryWait
  QC_CHECK_EQ(core.snapshot(kOemRecoveryQuietMs + 2600).state,
              CoordinatorState::RecoveryRetryWait);
  const auto before = core.snapshot(kOemRecoveryQuietMs + 2600).recovery;

  const auto effects =
      core.request_learn(LearnMode::Manual, kOemRecoveryQuietMs + 2650);
  QC_CHECK(refusal_reason(effects) == RefusalReason::AlreadyProvisioned);
  const auto after = core.snapshot(kOemRecoveryQuietMs + 2650);
  QC_CHECK_EQ(after.state, CoordinatorState::RecoveryRetryWait);
  QC_CHECK_EQ(after.recovery.phase, before.phase);
  QC_CHECK_EQ(after.recovery.due_ms, before.due_ms);
  QC_CHECK_EQ(after.recovery.queries_started, before.queries_started);

  // The retry must still fire at the preserved due time.
  core.poll(before.due_ms);
  const auto q2 = tx_request(core.poll(before.due_ms));
  QC_CHECK(q2.has_value());
  if (q2)
    QC_CHECK(q2->reason == TxReason::RecoveryQueryRetry);
}

QC_TEST("learn", "forget then learn is the explicit re-learn override") {
  auto core = provisioned_core();
  // Step 1: the deliberate override — Forget erases the binding.
  const auto forgotten = core.request_forget(1);
  bool erased = false;
  for (std::size_t index = 0; index < forgotten.size(); ++index)
    if (const auto* request =
            std::get_if<RequestPersistenceEffect>(&forgotten[index]))
      erased |= request->request.kind == PersistenceKind::EraseProvisioning;
  QC_CHECK(erased);
  QC_CHECK_EQ(core.snapshot(1).state, CoordinatorState::Unprovisioned);

  // Step 2: Learn now proceeds exactly as on a fresh device and can bind a
  // (different) fan.
  core.request_learn(LearnMode::Manual, 2);
  QC_CHECK_EQ(core.snapshot(2).state, CoordinatorState::LearningAwaitingFirst);
  const auto frame = command(0x40, 0x9F);
  core.on_frame(ByteView(frame), 3);
  core.on_frame(ByteView(frame), 604);
  const auto learned = core.on_frame(ByteView(frame), 1205);
  QC_CHECK_EQ(save_provisioning_count(learned), std::size_t{1});
  QC_CHECK_EQ(core.snapshot(1205).state, CoordinatorState::Idle);
}

}  // namespace
}  // namespace quietcool
