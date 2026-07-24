#include "quietcool/core/confirmation_core.h"
#include "support/test.h"

#include <optional>
#include <variant>

namespace quietcool {
namespace {

SenderId strand_sender() { return SenderId::from_be_u32(0xCB004739U).value(); }

std::optional<TxRequest> strand_tx(const CoreEffects& effects) {
  for (std::size_t i = 0; i < effects.size(); ++i)
    if (const auto* tx = std::get_if<RequestTxBurst>(&effects[i]))
      return tx->request;
  return std::nullopt;
}

ConfirmationCore strand_core() {
  ConfirmationCore core(CoreConfig{31});
  RestorableState restored;
  restored.sender = strand_sender();
  core.restore(restored, 0);
  return core;
}

// A full two-query OEM recovery cycle interrupted by fresh OEM activity during
// the second query's response window must still expire to Idle 30s after the
// latest OEM evidence (design 7.12: "Recovery expires after 30 seconds from the
// latest OEM evidence").  Before the fix, arm_from_oem_activity re-armed the
// spent cycle into RecoveryPhase::Complete and RecoveryScheduler::poll()
// short-circuited Complete before the expiry check, stranding the coordinator
// in RecoveryQuietWait forever.
QC_TEST("fable_strand",
        "OEM interrupting a spent recovery cycle still expires to Idle") {
  auto core = strand_core();
  const auto oem = FrameCodec::encode_query(strand_sender());

  core.on_frame(ByteView(oem.bytes), 0);           // -> OemHoldoff, arm cycle
  core.poll(kOemHoldoffMs);                         // -> RecoveryQuietWait
  core.poll(kOemRecoveryQuietMs);                   // -> RecoveryQueryPending
  const auto q1 = strand_tx(core.poll(kOemRecoveryQuietMs));
  QC_CHECK(q1.has_value());
  core.on_tx_started(q1->token, kOemRecoveryQuietMs);
  core.on_tx_complete(q1->token, kOemRecoveryQuietMs + 100);
  QC_CHECK_EQ(core.snapshot(kOemRecoveryQuietMs + 100).state,
              CoordinatorState::RecoveryResponseListening);

  core.poll(kOemRecoveryQuietMs + 1300);            // miss query 1 -> tail
  core.poll(kOemRecoveryQuietMs + 2600);            // tail expires -> RetryWait
  QC_CHECK_EQ(core.snapshot(kOemRecoveryQuietMs + 2600).state,
              CoordinatorState::RecoveryRetryWait);

  const auto due = core.snapshot(kOemRecoveryQuietMs + 2600).recovery.due_ms;
  core.poll(due);                                   // -> RecoveryQueryPending
  const auto q2 = strand_tx(core.poll(due));
  QC_CHECK(q2.has_value());
  core.on_tx_started(q2->token, due);               // queries_started == 2
  core.on_tx_complete(q2->token, due + 100);
  QC_CHECK_EQ(core.snapshot(due + 100).recovery.queries_started, 2U);

  core.on_frame(ByteView(oem.bytes), due + 200);    // fresh OEM -> OemHoldoff
  QC_CHECK_EQ(core.snapshot(due + 200).state, CoordinatorState::OemHoldoff);
  const auto expires = core.snapshot(due + 200).recovery.expires_ms;

  core.poll(due + 200 + kOemHoldoffMs);             // -> RecoveryQuietWait
  QC_CHECK_EQ(core.snapshot(due + 200 + kOemHoldoffMs).state,
              CoordinatorState::RecoveryQuietWait);

  core.poll(expires + 1);                           // past 30s: must expire
  const auto snapshot = core.snapshot(expires + 1);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::Idle);
  QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::Inactive);
}

}  // namespace
}  // namespace quietcool
