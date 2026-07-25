#include "quietcool/core/confirmation_core.h"
#include "quietcool/radio/burst_transmitter.h"
#include "support/test.h"
#include "support/test_doubles.h"

#include <optional>
#include <variant>

namespace quietcool {
namespace {

SenderId completion_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

ConfirmationCore completion_core() {
  ConfirmationCore core(CoreConfig{83});
  RestorableState restored;
  restored.sender = completion_sender();
  core.restore(restored, 0);
  return core;
}

std::optional<TxRequest> completion_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* request = std::get_if<RequestTxBurst>(&effects[index]))
      return request->request;
  return std::nullopt;
}

std::size_t completion_tx_count(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < effects.size(); ++index)
    count += std::holds_alternative<RequestTxBurst>(effects[index]);
  return count;
}

FrameBytes completion_frame(std::uint8_t state) {
  return {{0xCB, 0x00, 0x47, 0x39, state, state}};
}

TxRequest completion_start(ConfirmationCore& core, MonotonicMs now_ms) {
  const auto request = completion_tx(core.poll(now_ms));
  QC_CHECK(request.has_value());
  core.on_tx_started(request->token, now_ms);
  return *request;
}

QC_TEST("INV-02", "OFF has five nonrenewable refires and six spendable starts") {
  auto transaction = CommandTransaction::begin(
      TransactionId(1), FanState::command(Speed::Low, Duration::Off),
      std::nullopt);
  for (std::uint8_t attempt = 1; attempt <= 6; ++attempt) {
    QC_CHECK_EQ(transaction.note_command_burst_started().value(),
                AttemptNumber(attempt));
    QC_CHECK_EQ(transaction.remaining_refires(),
                RefireCount(static_cast<std::uint8_t>(6 - attempt)));
  }
  QC_CHECK(!transaction.may_emit_another_command());

  auto core = completion_core();
  core.request_state(FanState::command(Speed::Low, Duration::Off), 0);
  core.poll(0);
  core.poll(kTxLeaseStartWatchdogMs);
  QC_CHECK_EQ(core.snapshot(kTxLeaseStartWatchdogMs)
                  .transaction->attempts_started,
              0U);
}

QC_TEST("INV-03", "ON has four total starts and duplicate join does not renew") {
  auto transaction = CommandTransaction::begin(
      TransactionId(1), FanState::command(Speed::Low, Duration::Continuous),
      std::nullopt);
  transaction.note_command_burst_started();
  QC_CHECK_EQ(transaction.remaining_refires(), RefireCount(3));
  QC_CHECK_EQ(transaction.compare_request(
                  FanState::command(Speed::Low, Duration::Continuous)),
              JoinDecision::SemanticDuplicate);
  for (std::uint8_t attempt = 2; attempt <= 4; ++attempt)
    QC_CHECK(transaction.note_command_burst_started().has_value());
  QC_CHECK(!transaction.note_command_burst_started().has_value());
}

QC_TEST("INV-05", "unprovisioned and learning paths are RF silent") {
  ConfirmationCore core(CoreConfig{83});
  QC_CHECK_EQ(completion_tx_count(core.on_radio_ready(0)), 0U);
  QC_CHECK_EQ(completion_tx_count(core.request_state(
                  FanState::command(Speed::Low, Duration::Continuous), 1)),
              0U);
  QC_CHECK_EQ(completion_tx_count(core.request_manual_refresh(2)), 0U);
  QC_CHECK_EQ(completion_tx_count(core.poll(100000)), 0U);
  core.request_learn(LearnMode::Manual, 100001);
  const auto command = completion_frame(0x9F);
  QC_CHECK_EQ(completion_tx_count(
                  core.on_frame(ByteView(command.bytes), 100002)),
              0U);
  QC_CHECK_EQ(completion_tx_count(core.poll(200000)), 0U);
}

QC_TEST("INV-06", "one live burst prevents packet interleaving") {
  test::FakeClock clock;
  test::FakeRadio radio;
  BurstTransmitter transmitter(clock, radio);
  const TxRequest first{TxToken(1), FrameCodec::encode_query(completion_sender()),
                        TxReason::BootQuery, {}, {}, 0};
  auto second = first;
  second.token = TxToken(2);
  QC_CHECK_EQ(transmitter.accept(first), TxAcceptResult::Accepted);
  QC_CHECK_EQ(transmitter.accept(second), TxAcceptResult::Busy);
  transmitter.poll();
  QC_CHECK_EQ(transmitter.accept(second), TxAcceptResult::Busy);
}

void exhaust_with_fallbacks(FanState request, std::uint8_t expected_commands) {
  auto core = completion_core();
  core.request_state(request, 0);
  MonotonicMs command_ms = 0;
  for (std::uint8_t attempt = 0; attempt < expected_commands; ++attempt) {
    const auto command = completion_start(core, command_ms);
    QC_CHECK_EQ(command.reason, TxReason::TransactionCommand);
    core.on_tx_complete(command.token, command_ms + 400);
    core.poll(command_ms + 2001);
    core.poll(command_ms + 2901);
    const auto fallback = completion_start(core, command_ms + 2901);
    QC_CHECK_EQ(fallback.reason, TxReason::TransactionFallbackQuery);
    core.on_tx_complete(fallback.token, command_ms + 3001);
    core.poll(command_ms + 4002);
    core.poll(command_ms + 5402);
    command_ms += 5402;
  }
  const auto snapshot = core.snapshot(command_ms);
  QC_CHECK_EQ(snapshot.last_transaction_outcome.value(),
              TransactionOutcome::Exhausted);
  QC_CHECK_EQ(snapshot.logical_command_bursts, expected_commands);
  QC_CHECK_EQ(snapshot.logical_query_bursts, expected_commands);
}

QC_TEST("INV-07", "command and fallback RF stay within ON and OFF bounds") {
  exhaust_with_fallbacks(
      FanState::command(Speed::Low, Duration::Continuous), 4);
  exhaust_with_fallbacks(FanState::command(Speed::Low, Duration::Off), 6);
}

QC_TEST("INV-08", "one OEM cycle permits only initial and retry queries") {
  auto core = completion_core();
  const auto oem = FrameCodec::encode_query(completion_sender());
  core.on_frame(ByteView(oem.bytes), 0);
  core.poll(kOemHoldoffMs);
  core.poll(kOemRecoveryQuietMs);
  auto query = completion_start(core, kOemRecoveryQuietMs);
  core.on_tx_complete(query.token, kOemRecoveryQuietMs + 100);
  core.poll(kOemRecoveryQuietMs + kDirectQueryAcceptEndMs + 1);
  core.poll(kOemRecoveryQuietMs + kResponseTailEndMs + 1);
  const auto due = core.snapshot(kOemRecoveryQuietMs + kResponseTailEndMs + 1)
                       .recovery.due_ms;
  core.poll(due);
  query = completion_start(core, due);
  core.on_tx_complete(query.token, due + 100);
  core.poll(due + kDirectQueryAcceptEndMs + 1);
  core.poll(due + kResponseTailEndMs + 1);
  core.poll(due + 10000);
  const auto snapshot = core.snapshot(due + 10000);
  QC_CHECK_EQ(snapshot.logical_query_bursts, 2U);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::Idle);
  QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::Inactive);
  QC_CHECK(!snapshot.recovery.cause.has_value());
}

QC_TEST("INV-10", "transaction emits no query before its old tail expires") {
  auto core = completion_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto command = completion_start(core, 0);
  core.on_tx_complete(command.token, 400);
  QC_CHECK_EQ(completion_tx_count(core.poll(2001)), 0U);
  QC_CHECK_EQ(completion_tx_count(core.poll(2900)), 0U);
  QC_CHECK_EQ(core.snapshot(2900).logical_query_bursts, 0U);
  core.poll(2901);
  const auto query = completion_tx(core.poll(2901));
  QC_CHECK(query.has_value());
  QC_CHECK_EQ(query->reason, TxReason::TransactionFallbackQuery);
}

QC_TEST("INV-12", "partial consensus never crosses into fallback epoch") {
  auto core = completion_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto command = completion_start(core, 0);
  core.on_tx_complete(command.token, 400);
  const auto response = completion_frame(0xDF);
  core.on_frame(ByteView(response.bytes), 1105);
  core.poll(2001);
  core.poll(2901);
  const auto fallback = completion_start(core, 2901);
  core.on_tx_complete(fallback.token, 3001);
  core.on_frame(ByteView(response.bytes), 3201);
  QC_CHECK(!core.snapshot(3201).last_transaction_outcome.has_value());
  core.on_frame(ByteView(response.bytes), 3261);
  QC_CHECK_EQ(core.snapshot(3261).last_transaction_outcome.value(),
              TransactionOutcome::Confirmed);
}

QC_TEST("INV-13", "classification requires consensus before authority promotion") {
  auto core = completion_core();
  core.request_manual_refresh(0);
  const auto query = completion_start(core, 0);
  core.on_tx_complete(query.token, 100);
  const auto response = completion_frame(0xDF);
  const auto revision = core.snapshot(299).authority.revision;
  core.on_frame(ByteView(response.bytes), 300);
  QC_CHECK_EQ(core.snapshot(300).authority.revision, revision);
  QC_CHECK(!std::holds_alternative<ConfirmedStateAuthority>(
      core.snapshot(300).authority.state));
  core.on_frame(ByteView(response.bytes), 360);
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(
      core.snapshot(360).authority.state));
}

QC_TEST("INV-18", "late poll expires every response family before watchdogs") {
  {
    auto core = completion_core();
    core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
    const auto command = completion_start(core, 0);
    core.on_tx_complete(command.token, 400);
    core.poll(10000);
    QC_CHECK_EQ(core.snapshot(10000).state,
                CoordinatorState::PostCommandTailWait);
  }
  for (const QueryPurpose purpose : {QueryPurpose::Boot, QueryPurpose::Manual,
                                     QueryPurpose::Recovery}) {
    auto core = completion_core();
    MonotonicMs start = 0;
    if (purpose == QueryPurpose::Boot) core.on_radio_ready(0);
    else if (purpose == QueryPurpose::Manual) core.request_manual_refresh(0);
    else {
      const auto oem = FrameCodec::encode_query(completion_sender());
      core.on_frame(ByteView(oem.bytes), 0);
      core.poll(kOemHoldoffMs);
      core.poll(kOemRecoveryQuietMs);
      start = kOemRecoveryQuietMs;
    }
    const auto query = completion_start(core, start);
    core.on_tx_complete(query.token, start + 100);
    core.poll(start + 10000);
    QC_CHECK_EQ(core.snapshot(start + 10000).state,
                CoordinatorState::ResponseTailQuarantine);
  }
  {
    auto core = completion_core();
    core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
    const auto command = completion_start(core, 0);
    core.on_tx_complete(command.token, 400);
    core.poll(2001);
    core.poll(2901);
    const auto query = completion_start(core, 2901);
    core.on_tx_complete(query.token, 3001);
    core.poll(10000);
    QC_CHECK_EQ(core.snapshot(10000).state,
                CoordinatorState::ResponseTailQuarantine);
  }
}

QC_TEST("INV-23", "restore exposes hints but no volatile or confirmed work") {
  ConfirmationCore core(CoreConfig{83});
  RestorableState restored;
  restored.sender = completion_sender();
  restored.remembered_speed = Speed::High;
  restored.observation_hint =
      RestoredObservationHint{0x3F, SpeedCapability::Three};
  QC_CHECK_EQ(completion_tx_count(core.restore(restored, 0)), 0U);
  const auto snapshot = core.snapshot(0);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::Idle);
  QC_CHECK(!snapshot.transaction.has_value());
  QC_CHECK(!snapshot.live_tx.has_value());
  QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::Inactive);
  QC_CHECK(!snapshot.learning.active);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(snapshot.authority.state));
  QC_CHECK(std::holds_alternative<UnknownTimerAuthority>(snapshot.authority.timer));
}

}  // namespace
}  // namespace quietcool
