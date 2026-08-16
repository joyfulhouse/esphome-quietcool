#include "quietcool/core/confirmation_core.h"
#include "support/core_test_builder.h"
#include "support/test.h"

#include <optional>
#include <variant>

namespace quietcool {
namespace {

SenderId terminal_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

ConfirmationCore terminal_core() {
  ConfirmationCore core(CoreConfig{23});
  RestorableState restored;
  restored.sender = terminal_sender();
  core.restore(restored, 0);
  return core;
}

FrameBytes terminal_frame(std::uint8_t value) {
  return {{0xCB, 0x00, 0x47, 0x39, value, value}};
}

void seed_confirmed_authority(ConfirmationCore& core) {
  const auto response = terminal_frame(0x1F);
  core.on_frame(ByteView(response.bytes), 100);
  core.on_frame(ByteView(response.bytes),
                100 + kMinIndependentCandidateGapMs);
}

std::optional<AuthoritySnapshot> terminal_publication(
    const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* publication =
            std::get_if<PublishAuthorityEffect>(&effects[index]))
      return publication->authority;
  return std::nullopt;
}

void check_rearmed_progress(ConfirmationCore& core, MonotonicMs expiry_ms) {
  const auto effects = core.poll(expiry_ms);
  const auto publication = terminal_publication(effects);
  QC_CHECK(publication.has_value());
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(publication->state));
  const auto rearmed = core.snapshot(expiry_ms);
  QC_CHECK_EQ(rearmed.state, CoordinatorState::RecoveryQuietWait);
  QC_CHECK_EQ(rearmed.recovery.phase, RecoveryPhase::QuietWait);
  QC_CHECK_EQ(rearmed.recovery.cause_anchor_ms, expiry_ms);
  QC_CHECK_EQ(rearmed.recovery.due_ms,
              saturating_add(expiry_ms, kOemRecoveryQuietMs));
  core.poll(rearmed.recovery.due_ms);
  QC_CHECK_EQ(core.snapshot(rearmed.recovery.due_ms).state,
              CoordinatorState::RecoveryQueryPending);
}

QC_TEST("terminal_recovery_rearm",
        "lone ambiguous evidence re-arms a complete OEM cycle") {
  auto core = terminal_core();
  seed_confirmed_authority(core);
  ConfirmationCoreTestBuilder::complete_oem_recovery(core, 1000);
  QC_CHECK_EQ(core.snapshot(1000).recovery.phase, RecoveryPhase::Complete);

  const auto ambiguous = terminal_frame(0xBF);
  constexpr MonotonicMs heard_ms = 5000;
  core.on_frame(ByteView(ambiguous.bytes), heard_ms);
  check_rearmed_progress(core, heard_ms + kPassiveReplyAcceptEndMs + 1);
}

QC_TEST("terminal_recovery_rearm",
        "lone ambiguous evidence re-arms an expired OEM cycle") {
  auto core = terminal_core();
  seed_confirmed_authority(core);
  ConfirmationCoreTestBuilder::complete_oem_recovery(core, 1000);
  const auto terminal = core.snapshot(1000).recovery;
  const MonotonicMs heard_ms = terminal.expires_ms + 1;
  QC_CHECK_EQ(core.snapshot(heard_ms).recovery.phase, RecoveryPhase::Complete);
  QC_CHECK_EQ(core.poll(heard_ms).size(), 0U);

  const auto ambiguous = terminal_frame(0xBF);
  core.on_frame(ByteView(ambiguous.bytes), heard_ms);
  check_rearmed_progress(core, heard_ms + kPassiveReplyAcceptEndMs + 1);
}

}  // namespace
}  // namespace quietcool
