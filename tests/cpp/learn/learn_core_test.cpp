#include "quietcool/core/confirmation_core.h"
#include "support/test.h"

#include <array>
#include <cstddef>

// Core-level guard for issue #6: a second distinct fan heard during Learn must
// refuse and leave NVS untouched. The assertion that actually protects the
// binding is that NO SaveProvisioning effect is emitted on the ambiguous path.

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

}  // namespace
}  // namespace quietcool
