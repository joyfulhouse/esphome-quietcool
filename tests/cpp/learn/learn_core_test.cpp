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

ConfirmationCore provisioned_core() {
  ConfirmationCore core(CoreConfig{23});
  RestorableState restored;
  restored.sender = SenderId::from_be_u32(0xCB004739U).value();
  core.restore(restored, 0);
  return core;
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
