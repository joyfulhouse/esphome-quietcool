#include "quietcool/core/confirmation_core.h"
#include "support/recursion_probe.h"
#include "support/test.h"

#include <array>
#include <optional>
#include <variant>

namespace quietcool {
namespace {

SenderId sender() { return SenderId::from_be_u32(0xCB004739U).value(); }
FrameBytes frame(std::uint8_t state) {
  return {{0xCB, 0x00, 0x47, 0x39, state, state}};
}

std::optional<TxRequest> requested_tx(const CoreEffects& effects) {
  for (std::size_t i = 0; i < effects.size(); ++i) {
    if (const auto* value = std::get_if<RequestTxBurst>(&effects[i]))
      return value->request;
  }
  return std::nullopt;
}

std::size_t tx_count(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < effects.size(); ++i)
    if (std::holds_alternative<RequestTxBurst>(effects[i])) ++count;
  return count;
}

ConfirmationCore provisioned() {
  ConfirmationCore core(CoreConfig{17});
  RestorableState restored;
  restored.sender = sender();
  core.restore(restored, 0);
  return core;
}

TxRequest lease_and_start(ConfirmationCore& core, MonotonicMs lease_ms,
                          MonotonicMs start_ms) {
  const auto request = requested_tx(core.poll(lease_ms));
  QC_CHECK(request.has_value());
  core.on_tx_started(request->token, start_ms);
  return *request;
}

void consensus(ConfirmationCore& core, std::uint8_t state,
               MonotonicMs first_ms) {
  const auto value = frame(state);
  core.on_frame(ByteView(value.bytes), first_ms);
  core.on_frame(ByteView(value.bytes), first_ms + 60);
}

QC_TEST("control", "provisioned boot emits once and repeated ready is silent") {
  auto core = provisioned();
  QC_CHECK_EQ(core.snapshot(0).state, CoordinatorState::Idle);
  QC_CHECK_EQ(tx_count(core.on_radio_ready(0)), 0U);
  QC_CHECK_EQ(core.snapshot(0).state, CoordinatorState::BootQueryPending);
  const auto query = requested_tx(core.poll(0));
  QC_CHECK(query.has_value());
  QC_CHECK_EQ(query->reason, TxReason::BootQuery);
  QC_CHECK_EQ(query->payload, FrameCodec::encode_query(sender()));
  QC_CHECK_EQ(tx_count(core.on_radio_ready(1)), 0U);
}

QC_TEST("control", "unprovisioned boot and restore are RF silent") {
  ConfirmationCore core(CoreConfig{1});
  RestorableState restored;
  restored.observation_hint = RestoredObservationHint{0x3F,
                                                       SpeedCapability::Three};
  QC_CHECK_EQ(tx_count(core.restore(restored, 0)), 0U);
  QC_CHECK_EQ(tx_count(core.on_radio_ready(1)), 0U);
  QC_CHECK_EQ(core.snapshot(1).state, CoordinatorState::Unprovisioned);
  QC_CHECK_EQ(tx_count(core.request_state(
                  FanState::command(Speed::Low, Duration::Continuous), 2)),
              0U);
}

QC_TEST("REG-D", "unsolicited report confirms with no query") {
  auto core = provisioned();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto command = lease_and_start(core, 0, 0);
  QC_CHECK_EQ(command.reason, TxReason::TransactionCommand);
  core.on_tx_complete(command.token, 400);
  QC_CHECK_EQ(core.snapshot(400).state, CoordinatorState::PostCommandListening);
  test::start_recursion_probe();
  consensus(core, 0xDF, 1105);
  const bool recursive_consensus_path = test::stop_recursion_probe();
  QC_CHECK(!recursive_consensus_path);
  const auto snapshot = core.snapshot(1165);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::ResponseTailQuarantine);
  QC_CHECK_EQ(snapshot.last_transaction_outcome.value(),
              TransactionOutcome::Confirmed);
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(
      snapshot.authority.state));
  QC_CHECK_EQ(std::get<ConfirmedStateAuthority>(snapshot.authority.state).source,
              EvidenceSource::PostCommandConsensus);
  QC_CHECK_EQ(snapshot.logical_command_bursts, 1U);
  QC_CHECK_EQ(snapshot.logical_query_bursts, 0U);
  core.on_frame(ByteView(frame(0xDF).bytes), 1265);
  QC_CHECK_EQ(core.snapshot(1265).state,
              CoordinatorState::ResponseTailQuarantine);
}

QC_TEST("REG-R4-REFRESH", "Refresh at plus 800 is inert and refused") {
  auto core = provisioned();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto command = lease_and_start(core, 0, 0);
  core.on_tx_complete(command.token, 400);
  const auto before = core.snapshot(1199);
  const auto effects = core.request_manual_refresh(1200);
  QC_CHECK_EQ(tx_count(effects), 0U);
  QC_CHECK_EQ(core.snapshot(1200).state, CoordinatorState::PostCommandListening);
  QC_CHECK_EQ(core.snapshot(1200).authority.revision, before.authority.revision);
  QC_CHECK_EQ(core.snapshot(1200).transaction->prior_authority,
              before.transaction->prior_authority);
  consensus(core, 0xDF, 1201);
  QC_CHECK_EQ(core.snapshot(1261).last_transaction_outcome.value(),
              TransactionOutcome::Confirmed);
}

QC_TEST("REG-R4-STALL", "late poll preserves all five OFF refires") {
  auto core = provisioned();
  core.request_state(FanState::command(Speed::Low, Duration::Off), 0);
  const auto command = lease_and_start(core, 0, 0);
  core.on_tx_complete(command.token, 400);
  const auto effects = core.poll(5400);
  QC_CHECK_EQ(tx_count(effects), 0U);
  const auto snapshot = core.snapshot(5400);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::PostCommandTailWait);
  QC_CHECK(snapshot.transaction.has_value());
  QC_CHECK_EQ(snapshot.transaction->remaining_refires, RefireCount(5));
  QC_CHECK(!snapshot.last_transaction_outcome.has_value());
  core.poll(5400);
  QC_CHECK_EQ(core.snapshot(5400).state, CoordinatorState::FallbackQueryPending);
}

QC_TEST("REG-B", "OEM timer separates state and remaining-time authority") {
  auto core = provisioned();
  core.request_manual_refresh(0);
  const auto query = lease_and_start(core, 10, 10);
  core.on_tx_complete(query.token, 400);
  consensus(core, 0xF1, 427);
  const auto authority = core.snapshot(487).authority;
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(authority.state));
  QC_CHECK(std::get<ConfirmedStateAuthority>(authority.state).state.is_on());
  QC_CHECK(std::holds_alternative<ProgrammedDurationAuthority>(authority.timer));
}

QC_TEST("REG-A", "prior-state echo retries instead of yielding") {
  auto core = provisioned();
  core.request_manual_refresh(0);
  const auto query = lease_and_start(core, 10, 10);
  core.on_tx_complete(query.token, 400);
  consensus(core, 0xDF, 427);
  core.poll(2511);
  QC_CHECK_EQ(core.snapshot(2511).state, CoordinatorState::Idle);

  core.request_state(FanState::command(Speed::High, Duration::Continuous), 10000);
  const auto first = lease_and_start(core, 10000, 10000);
  core.on_tx_complete(first.token, 10400);
  consensus(core, 0x9F, 11105);
  auto snapshot = core.snapshot(11165);
  QC_CHECK(!snapshot.last_transaction_outcome.has_value());
  QC_CHECK(snapshot.transaction.has_value());
  QC_CHECK_EQ(snapshot.state, CoordinatorState::ResponseTailQuarantine);
  core.poll(12901);
  const auto second = lease_and_start(core, 12901, 12901);
  core.on_tx_complete(second.token, 13301);
  consensus(core, 0xFF, 14006);
  QC_CHECK_EQ(core.snapshot(14066).last_transaction_outcome.value(),
              TransactionOutcome::Confirmed);
}

QC_TEST("REG-TAIL", "partial old tail cannot seed fallback consensus") {
  auto core = provisioned();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto command = lease_and_start(core, 0, 0);
  core.on_tx_complete(command.token, 400);
  core.on_frame(ByteView(frame(0xDF).bytes), 1105);
  core.on_frame(ByteView(frame(0xDF).bytes), 2899);
  QC_CHECK_EQ(core.snapshot(2899).state, CoordinatorState::PostCommandTailWait);
  core.poll(2901);
  const auto query = lease_and_start(core, 2901, 2901);
  core.on_tx_complete(query.token, 3000);
  core.on_frame(ByteView(frame(0xDF).bytes), 3201);
  QC_CHECK_EQ(core.snapshot(3201).state,
              CoordinatorState::FallbackResponseListening);
  core.on_frame(ByteView(frame(0xDF).bytes), 3261);
  QC_CHECK_EQ(core.snapshot(3261).last_transaction_outcome.value(),
              TransactionOutcome::Confirmed);
}

QC_TEST("REG-C", "repeated OEM traffic keeps bridge silent") {
  auto core = provisioned();
  const auto query = FrameCodec::encode_query(sender());
  for (MonotonicMs now = 0; now <= 11000; now += 1000) {
    QC_CHECK_EQ(tx_count(core.on_frame(ByteView(query.bytes), now)), 0U);
    QC_CHECK_EQ(tx_count(core.poll(now)), 0U);
  }
  QC_CHECK_EQ(tx_count(core.poll(13999)), 0U);
  core.poll(14000);
  QC_CHECK_EQ(core.snapshot(14000).state, CoordinatorState::RecoveryQueryPending);
  QC_CHECK(requested_tx(core.poll(14000)).has_value());
}

QC_TEST("recovery", "missed initial query quarantines its tail before retry wait") {
  auto core = provisioned();
  const auto oem = FrameCodec::encode_query(sender());
  core.on_frame(ByteView(oem.bytes), 0);
  core.poll(kOemHoldoffMs);
  core.poll(kOemRecoveryQuietMs);
  QC_CHECK_EQ(core.snapshot(kOemRecoveryQuietMs).state,
              CoordinatorState::RecoveryQueryPending);

  const auto query = lease_and_start(core, kOemRecoveryQuietMs,
                                     kOemRecoveryQuietMs);
  core.on_tx_complete(query.token, kOemRecoveryQuietMs + 100);
  core.poll(kOemRecoveryQuietMs + kDirectQueryAcceptEndMs + 1);

  const auto tail = core.snapshot(kOemRecoveryQuietMs +
                                  kDirectQueryAcceptEndMs + 1);
  QC_CHECK_EQ(tail.state, CoordinatorState::ResponseTailQuarantine);
  QC_CHECK(std::holds_alternative<BeginRecoveryRetryWait>(
      std::get<TailQuarantineContext>(tail.context).exit));

  core.poll(kOemRecoveryQuietMs + kResponseTailEndMs + 1);
  QC_CHECK_EQ(core.snapshot(kOemRecoveryQuietMs + kResponseTailEndMs + 1).state,
              CoordinatorState::RecoveryRetryWait);
}

}  // namespace
}  // namespace quietcool
