// A 0xCE special response was classified as SpecialDiagnostic and then dropped:
// on_frame had no arm for the variant, so it fell through to `return {}` with
// no event. This wires it to a log-only diagnostic event with a hard
// constraint: zero effect on authority or actuation. The test pins that
// constraint so the wiring can never quietly grow into a state input.

#include "quietcool/core/confirmation_core.h"
#include "support/test.h"

#include <array>
#include <cstdint>
#include <variant>

namespace quietcool {
namespace {

SenderId sender() { return SenderId::from_be_u32(0xCB004739U).value(); }

ConfirmationCore provisioned() {
  ConfirmationCore core(CoreConfig{17});
  RestorableState restored;
  restored.sender = sender();
  core.restore(restored, 0);
  return core;
}

bool has_diagnostic(const CoreEffects& effects) {
  for (std::size_t i = 0; i < effects.size(); ++i) {
    const auto* event = std::get_if<PublishCoreEvent>(&effects[i]);
    if (event && event->event.kind == CoreEventKind::Diagnostic) return true;
  }
  return false;
}

// Any effect that could move authority or the radio. A log-only diagnostic must
// emit none of these.
bool touches_authority_or_actuation(const CoreEffects& effects) {
  for (std::size_t i = 0; i < effects.size(); ++i) {
    if (std::holds_alternative<PublishAuthorityEffect>(effects[i]) ||
        std::holds_alternative<RequestPersistenceEffect>(effects[i]) ||
        std::holds_alternative<RequestTxBurst>(effects[i]) ||
        std::holds_alternative<RevokeTxLease>(effects[i]) ||
        std::holds_alternative<RequestRadioReset>(effects[i]))
      return true;
  }
  return false;
}

QC_TEST("special_diagnostic", "0xCE special response is logged with no authority effect") {
  auto core = provisioned();
  QC_CHECK_EQ(core.snapshot(0).state, CoordinatorState::Idle);
  const auto before = core.snapshot(10'000).authority.revision;

  const std::array<std::uint8_t, 6> special{0xCE, 0x00, 0x47, 0x39, 0xDF, 0xDF};
  const auto effects = core.on_frame(ByteView(special), 10'000);

  QC_CHECK(has_diagnostic(effects));
  // Hard constraint: zero effect on authority or actuation. The revision
  // counter is bumped by every promote/invalidate, so an unchanged revision
  // proves the special frame was never treated as state.
  QC_CHECK(!touches_authority_or_actuation(effects));
  QC_CHECK_EQ(core.snapshot(10'000).state, CoordinatorState::Idle);
  QC_CHECK_EQ(core.snapshot(10'000).authority.revision, before);
}

}  // namespace
}  // namespace quietcool
