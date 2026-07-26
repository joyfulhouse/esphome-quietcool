#include "quietcool/core/confirmation_core.h"
#include "support/test.h"

#include <array>
#include <cstddef>
#include <optional>

// Core-level guard for issue #6: a second distinct fan heard during Learn must
// refuse and leave NVS untouched. The assertion that actually protects the
// binding is that NO SaveProvisioning effect is emitted on the ambiguous path.

namespace quietcool {
namespace {

constexpr std::array<std::uint8_t, 6> command(std::uint8_t id,
                                              std::uint8_t state) {
  return {0xCB, 0x00, 0x47, id, state, state};
}

std::optional<PersistenceRequest> save_provisioning_request(
    const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index) {
    const auto* request = std::get_if<RequestPersistenceEffect>(&effects[index]);
    if (request && request->request.kind == PersistenceKind::SaveProvisioning)
      return request->request;
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

std::size_t save_provisioning_count(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < effects.size(); ++index) {
    const auto* request = std::get_if<RequestPersistenceEffect>(&effects[index]);
    count += request &&
             request->request.kind == PersistenceKind::SaveProvisioning;
  }
  return count;
}

std::optional<RefusalReason> refusal_reason(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* refused = std::get_if<RefusedInput>(&effects[index]))
      return refused->reason;
  return std::nullopt;
}

ConfirmationCore provisioned_core(
    std::optional<SpeedCapability> capability = std::nullopt) {
  ConfirmationCore core(CoreConfig{23});
  RestorableState restored;
  restored.sender = SenderId::from_be_u32(0xCB004739U).value();
  restored.speed_capability = capability;
  core.restore(restored, 0);
  return core;
}

// Drives a full Learned binding: three independent sightings of one fan
// (kLearnMinSightings, spaced by >= kLearnSightingGapMs). Returns the effects
// of the binding frame.
CoreEffects learn_fan(ConfirmationCore& core, std::uint8_t id) {
  core.request_learn(LearnMode::Manual, 0);
  const auto frame = command(id, 0x9F);
  core.on_frame(ByteView(frame), 1);
  core.on_frame(ByteView(frame), 700);
  return core.on_frame(ByteView(frame), 1400);
}

QC_TEST("learn", "ambiguous relearn keeps the bound fan and writes no NVS") {
  auto core = provisioned_core();
  core.request_learn(LearnMode::Manual, 0);
  QC_CHECK_EQ(core.snapshot(0).state, CoordinatorState::LearningAwaitingFirst);

  const auto ours = command(0x39, 0x9F);      // the already-bound downstairs fan
  const auto intruder = command(0x40, 0xAF);  // a second fan on the same air
  core.on_frame(ByteView(ours), 1);
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

// Issue #31: the sticky speed capability is a property of the BOUND fan. Learning
// a different fan must drop it — in the snapshot (it re-aims commands and feeds
// get_traits) and in the SaveProvisioning request (the adapter re-encodes the
// whole record under the new sender, so a stale value would outlive reboots).
QC_TEST("learn", "learning a different fan clears the sticky capability") {
  auto core = provisioned_core(SpeedCapability::Two);
  QC_CHECK_EQ(core.snapshot(0).authority.speed_capability.value(),
              SpeedCapability::Two);

  const auto effects = learn_fan(core, 0x40);  // a different fan
  QC_CHECK_EQ(core.snapshot(1400).state, CoordinatorState::Idle);
  QC_CHECK(!core.snapshot(1400).authority.speed_capability.has_value());

  const auto save = save_provisioning_request(effects);
  QC_CHECK(save.has_value());
  QC_CHECK_EQ(save->sender->as_be_u32(), 0xCB004740U);
  QC_CHECK(!save->speed_capability.has_value());

  // The clear must also be PUBLISHED (issue #31 review): the fan entity's
  // cached speed count is seeded from published snapshots only, and without
  // this publication the old fan's band would keep mapping Home Assistant
  // level presses (and get_traits) until the new fan's first confirmed report.
  const auto published = published_authority(effects);
  QC_CHECK(published.has_value());
  QC_CHECK(!published->speed_capability.has_value());
}

QC_TEST("learn", "re-learning the same fan keeps the sticky capability") {
  auto core = provisioned_core(SpeedCapability::Two);
  const auto effects = learn_fan(core, 0x39);  // the already-bound fan
  QC_CHECK_EQ(core.snapshot(1400).state, CoordinatorState::Idle);
  QC_CHECK_EQ(core.snapshot(1400).authority.speed_capability.value(),
              SpeedCapability::Two);

  const auto save = save_provisioning_request(effects);
  QC_CHECK(save.has_value());
  QC_CHECK_EQ(save->speed_capability.value(), SpeedCapability::Two);

  // Same-fan re-learn republishes the surviving capability unchanged.
  const auto published = published_authority(effects);
  QC_CHECK(published.has_value());
  QC_CHECK_EQ(published->speed_capability.value(), SpeedCapability::Two);
}

}  // namespace
}  // namespace quietcool
