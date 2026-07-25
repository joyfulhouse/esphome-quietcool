#include "quietcool/core/confirmation_core.h"
#include "support/test.h"

#include <optional>
#include <variant>

namespace quietcool {
namespace {

SenderId tail_matrix_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

ConfirmationCore tail_matrix_core() {
  ConfirmationCore core(CoreConfig{103});
  RestorableState restored;
  restored.sender = tail_matrix_sender();
  core.restore(restored, 0);
  return core;
}

std::optional<TxRequest> tail_matrix_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* request = std::get_if<RequestTxBurst>(&effects[index]))
      return request->request;
  return std::nullopt;
}

FrameBytes tail_matrix_frame(std::uint8_t raw_state) {
  return {{0xCB, 0x00, 0x47, 0x39, raw_state, raw_state}};
}

TxRequest tail_matrix_start(ConfirmationCore& core, MonotonicMs now_ms) {
  const auto request = tail_matrix_tx(core.poll(now_ms));
  QC_CHECK(request.has_value());
  core.on_tx_started(request->token, now_ms);
  return *request;
}

void tail_matrix_consensus(ConfirmationCore& core, std::uint8_t raw_state,
                           MonotonicMs first_ms) {
  const auto frame = tail_matrix_frame(raw_state);
  core.on_frame(ByteView(frame.bytes), first_ms);
  core.on_frame(ByteView(frame.bytes),
                first_ms + kMinIndependentCandidateGapMs);
}

TailExit tail_exit(const ConfirmationCore& core, MonotonicMs now_ms) {
  return std::get<TailQuarantineContext>(core.snapshot(now_ms).context).exit;
}

QC_TEST("transition_table", "every typed TailExit applies its named route") {
  {
    auto core = tail_matrix_core();
    core.request_manual_refresh(0);
    const auto query = tail_matrix_start(core, 0);
    core.on_tx_complete(query.token, 100);
    tail_matrix_consensus(core, 0xDF, 300);
    QC_CHECK(std::holds_alternative<ReturnIdle>(tail_exit(core, 360)));
    core.poll(kResponseTailEndMs + 1);
    QC_CHECK_EQ(core.snapshot(kResponseTailEndMs + 1).state,
                CoordinatorState::Idle);
  }
  {
    auto core = tail_matrix_core();
    core.request_state(
        FanState::command(Speed::Low, Duration::Continuous), 0);
    const auto command = tail_matrix_start(core, 0);
    core.on_tx_complete(command.token, 400);
    tail_matrix_consensus(core, 0xC0, 1105);
    QC_CHECK(std::holds_alternative<BeginRetryForTransaction>(
        tail_exit(core, 1165)));
    core.poll(400 + kResponseTailEndMs + 1);
    QC_CHECK_EQ(core.snapshot(2901).state, CoordinatorState::CommandPending);
  }
  {
    auto core = tail_matrix_core();
    core.request_manual_refresh(0);
    const auto query = tail_matrix_start(core, 0);
    core.on_tx_complete(query.token, 100);
    tail_matrix_consensus(core, 0xDF, 300);
    const auto replacement =
        FanState::command(Speed::High, Duration::Continuous);
    core.request_state(replacement, 400);
    QC_CHECK(std::holds_alternative<BeginDeferredCommand>(
        tail_exit(core, 400)));
    core.poll(kResponseTailEndMs + 1);
    const auto snapshot = core.snapshot(kResponseTailEndMs + 1);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
    QC_CHECK_EQ(snapshot.transaction->requested, replacement);
  }
  {
    auto core = tail_matrix_core();
    core.request_state(
        FanState::command(Speed::Low, Duration::Continuous), 0);
    const auto command = tail_matrix_start(core, 0);
    core.on_tx_complete(command.token, 400);
    tail_matrix_consensus(core, 0xBF, 1105);
    QC_CHECK(std::holds_alternative<BeginRecoveryQuietWait>(
        tail_exit(core, 1165)));
    core.poll(400 + kResponseTailEndMs + 1);
    QC_CHECK_EQ(core.snapshot(2901).state,
                CoordinatorState::RecoveryQuietWait);
  }
  {
    auto core = tail_matrix_core();
    const auto oem = FrameCodec::encode_query(tail_matrix_sender());
    core.on_frame(ByteView(oem.bytes), 0);
    core.poll(kOemHoldoffMs);
    core.poll(kOemRecoveryQuietMs);
    const auto query = tail_matrix_start(core, kOemRecoveryQuietMs);
    core.on_tx_complete(query.token, kOemRecoveryQuietMs + 100);
    core.poll(kOemRecoveryQuietMs + kDirectQueryAcceptEndMs + 1);
    QC_CHECK(std::holds_alternative<BeginRecoveryRetryWait>(
        tail_exit(core, kOemRecoveryQuietMs + kDirectQueryAcceptEndMs + 1)));
    core.poll(kOemRecoveryQuietMs + kResponseTailEndMs + 1);
    QC_CHECK_EQ(core.snapshot(
                    kOemRecoveryQuietMs + kResponseTailEndMs + 1).state,
                CoordinatorState::RecoveryRetryWait);
  }
}

}  // namespace
}  // namespace quietcool
