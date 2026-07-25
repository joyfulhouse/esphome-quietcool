#include "quietcool/core/confirmation_core.h"
#include "support/core_test_builder.h"
#include "support/test.h"

#include <array>
#include <limits>
#include <optional>
#include <variant>

namespace quietcool {
namespace {

SenderId invariant_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}
FrameBytes invariant_frame(std::uint8_t state) {
  return {{0xCB, 0x00, 0x47, 0x39, state, state}};
}
std::optional<TxRequest> tx_from(const CoreEffects& effects) {
  for (std::size_t i = 0; i < effects.size(); ++i)
    if (const auto* tx = std::get_if<RequestTxBurst>(&effects[i])) return tx->request;
  return std::nullopt;
}
std::size_t emitted_tx(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < effects.size(); ++i)
    count += std::holds_alternative<RequestTxBurst>(effects[i]);
  return count;
}
std::size_t emitted_event(const CoreEffects& effects, CoreEventKind kind) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < effects.size(); ++i) {
    const auto* event = std::get_if<PublishCoreEvent>(&effects[i]);
    if (event && event->event.kind == kind) ++count;
  }
  return count;
}
ConfirmationCore ready_core() {
  ConfirmationCore core(CoreConfig{23});
  RestorableState restore;
  restore.sender = invariant_sender();
  core.restore(restore, 0);
  return core;
}
TxRequest start_tx(ConfirmationCore& core, MonotonicMs now) {
  const auto tx = tx_from(core.poll(now));
  QC_CHECK(tx.has_value());
  core.on_tx_started(tx->token, now);
  return *tx;
}
void two_frames(ConfirmationCore& core, std::uint8_t state,
                MonotonicMs now) {
  const auto bytes = invariant_frame(state);
  core.on_frame(ByteView(bytes.bytes), now);
  core.on_frame(ByteView(bytes.bytes), now + 60);
}

QC_TEST("INV-14", "post-command frames at 0 and 399 are ignored") {
  for (const MonotonicMs age : {0U, 399U}) {
    auto core = ready_core();
    core.request_state(FanState::command(Speed::Low, Duration::Off), 0);
    const auto tx = start_tx(core, 0);
    core.on_tx_complete(tx.token, 400);
    const auto before = core.snapshot(400);
    const auto effects =
        core.on_frame(ByteView(invariant_frame(0xDF).bytes), 400 + age);
    const auto after = core.snapshot(400 + age);
    QC_CHECK_EQ(effects.size(), 1U);
    QC_CHECK_EQ(emitted_event(effects, CoreEventKind::Diagnostic), 1U);
    QC_CHECK_EQ(after.state, CoordinatorState::PostCommandListening);
    QC_CHECK_EQ(after.transaction->attempts_started,
                before.transaction->attempts_started);
    QC_CHECK_EQ(after.transaction->remaining_refires, RefireCount(5));
    QC_CHECK_EQ(after.logical_command_bursts, before.logical_command_bursts);
    QC_CHECK_EQ(after.logical_query_bursts, 0U);
    QC_CHECK_EQ(after.authority.revision, before.authority.revision);
    QC_CHECK_EQ(after.authority.last_diagnostic,
                before.authority.last_diagnostic);
    QC_CHECK_EQ(after.recovery.phase, before.recovery.phase);
    QC_CHECK_EQ(after.recovery.queries_started,
                before.recovery.queries_started);
  }
}

QC_TEST("INV-04", "exact OEM query cancels active and leased work") {
  auto core = ready_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto lease = tx_from(core.poll(0));
  QC_CHECK(lease.has_value());
  const auto oem = FrameCodec::encode_query(invariant_sender());
  const auto effects = core.on_frame(ByteView(oem.bytes), 1);
  QC_CHECK_EQ(core.snapshot(1).state, CoordinatorState::OemHoldoff);
  QC_CHECK_EQ(core.snapshot(1).last_transaction_outcome.value(),
              TransactionOutcome::CancelledByExactOemQuery);
  QC_CHECK(!core.snapshot(1).live_tx.has_value());
  QC_CHECK_EQ(emitted_tx(effects), 0U);
}

QC_TEST("INV-19 INV-20", "only matching started token spends budget") {
  auto core = ready_core();
  core.request_state(FanState::command(Speed::Low, Duration::Off), 0);
  const auto lease = tx_from(core.poll(0));
  QC_CHECK(lease.has_value());
  core.on_tx_started(TxToken(999), 1);
  QC_CHECK_EQ(core.snapshot(1).transaction->remaining_refires, RefireCount(5));
  core.on_tx_complete(TxToken(999), 2);
  QC_CHECK_EQ(core.snapshot(2).state, CoordinatorState::CommandLeaseIssued);
  core.on_tx_started(lease->token, 3);
  QC_CHECK_EQ(core.snapshot(3).transaction->attempts_started, 1U);
  core.on_tx_started(lease->token, 4);
  QC_CHECK_EQ(core.snapshot(4).transaction->attempts_started, 1U);
}

QC_TEST("INV-17", "ON plus OFF mismatch with attempts retries without promotion") {
  auto core = ready_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto tx = start_tx(core, 0);
  core.on_tx_complete(tx.token, 400);
  two_frames(core, 0xC0, 1105);
  const auto snapshot = core.snapshot(1165);
  QC_CHECK(snapshot.transaction.has_value());
  QC_CHECK_EQ(snapshot.state, CoordinatorState::ResponseTailQuarantine);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(snapshot.authority.state));
}

QC_TEST("INV-01", "OFF command-shaped mismatch re-aims and never yields") {
  auto core = ready_core();
  core.request_state(FanState::command(Speed::Low, Duration::Off), 0);
  const auto tx = start_tx(core, 0);
  core.on_tx_complete(tx.token, 400);
  two_frames(core, 0xBF, 1105);
  const auto snapshot = core.snapshot(1165);
  QC_CHECK(snapshot.transaction.has_value());
  QC_CHECK(!snapshot.last_transaction_outcome.has_value());
  QC_CHECK_EQ(snapshot.transaction->outbound_command,
              FanState::command(Speed::High, Duration::Off));
}

QC_TEST("INV-11", "fallback miss cannot recursively create another fallback") {
  auto core = ready_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto command = start_tx(core, 0);
  core.on_tx_complete(command.token, 400);
  core.poll(2001);
  core.poll(2901);
  const auto fallback = start_tx(core, 2901);
  QC_CHECK_EQ(fallback.reason, TxReason::TransactionFallbackQuery);
  core.on_tx_complete(fallback.token, 3000);
  core.poll(4002);
  QC_CHECK_EQ(core.snapshot(4002).state,
              CoordinatorState::ResponseTailQuarantine);
  core.poll(5402);
  const auto snapshot = core.snapshot(5402);
  QC_CHECK(snapshot.state == CoordinatorState::CommandPending ||
           snapshot.state == CoordinatorState::RetryDelay);
  QC_CHECK_EQ(snapshot.logical_query_bursts, 1U);
}

QC_TEST("INV-15", "OFF retry may promote non-command timer mismatch") {
  auto core = ready_core();
  core.request_state(FanState::command(Speed::Low, Duration::Off), 0);
  const auto tx = start_tx(core, 0);
  core.on_tx_complete(tx.token, 400);
  two_frames(core, 0xD1, 1105);
  const auto snapshot = core.snapshot(1165);
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(snapshot.authority.state));
  QC_CHECK(std::holds_alternative<ProgrammedDurationAuthority>(snapshot.authority.timer));
  QC_CHECK(snapshot.transaction.has_value());
}

QC_TEST("INV-16", "local timer expiry schedules one fresh one-shot query") {
  auto core = ready_core();
  core.request_state(FanState::command(Speed::Low, Duration::Hours1), 0);
  const auto command = start_tx(core, 0);
  core.on_tx_complete(command.token, 400);
  two_frames(core, 0xD1, 1105);
  core.poll(2901);
  QC_CHECK_EQ(core.snapshot(2901).state, CoordinatorState::Idle);
  QC_CHECK_EQ(emitted_tx(core.poll(3600400)), 0U);
  auto snapshot = core.snapshot(3600400);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::RecoveryQuietWait);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(snapshot.authority.state));
  const auto due = snapshot.recovery.due_ms;
  QC_CHECK(due >= 3600900 && due <= 3601400);
  core.poll(due);
  QC_CHECK_EQ(core.snapshot(due).state, CoordinatorState::RecoveryQueryPending);
  const auto query = tx_from(core.poll(due));
  QC_CHECK(query.has_value());
  QC_CHECK_EQ(query->reason, TxReason::TimerExpiryRecoveryQuery);
}

QC_TEST("INV-16", "late timer poll anchors freshness to the actual deadline") {
  auto core = ready_core();
  core.request_state(FanState::command(Speed::Low, Duration::Hours1), 0);
  const auto command = start_tx(core, 0);
  core.on_tx_complete(command.token, 400);
  two_frames(core, 0xD1, 1105);
  core.poll(2901);

  constexpr MonotonicMs deadline = 3600400;
  constexpr MonotonicMs late_poll = deadline + kTimerExpiryRecoveryMaxAgeMs + 1;
  core.poll(late_poll);
  const auto armed = core.snapshot(late_poll);
  QC_CHECK_EQ(armed.recovery.cause_anchor_ms, deadline);
  QC_CHECK_EQ(armed.recovery.expires_ms,
              deadline + kTimerExpiryRecoveryMaxAgeMs);

  QC_CHECK_EQ(emitted_tx(core.poll(late_poll)), 0U);
  const auto expired = core.snapshot(late_poll);
  QC_CHECK_EQ(expired.state, CoordinatorState::Idle);
  QC_CHECK_EQ(expired.recovery.phase, RecoveryPhase::Inactive);
  QC_CHECK_EQ(expired.logical_query_bursts, 0U);
}

QC_TEST("INV-22", "learning cancels work and remains receive-only") {
  auto core = ready_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto effects = core.request_learn(LearnMode::Manual, 1);
  QC_CHECK_EQ(emitted_tx(effects), 0U);
  QC_CHECK_EQ(core.snapshot(1).state, CoordinatorState::LearningAwaitingFirst);
  const auto first = invariant_frame(0x9F);
  core.on_frame(ByteView(first.bytes), 10);
  QC_CHECK_EQ(core.snapshot(10).state, CoordinatorState::LearningAwaitingSecond);
  // Issue #6: binding now requires three independent sightings, not two.
  core.on_frame(ByteView(first.bytes), 611);
  QC_CHECK_EQ(core.snapshot(611).state,
              CoordinatorState::LearningAwaitingSecond);
  core.on_frame(ByteView(first.bytes), 1212);
  QC_CHECK_EQ(core.snapshot(1212).state, CoordinatorState::Idle);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(
      core.snapshot(1212).authority.state));
}

QC_TEST("INV-21", "backward response time cannot confirm or extend epoch") {
  auto core = ready_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 1000);
  const auto tx = start_tx(core, 1000);
  core.on_tx_complete(tx.token, 1400);
  core.on_frame(ByteView(invariant_frame(0xDF).bytes), 2105);
  core.on_frame(ByteView(invariant_frame(0xDF).bytes), 2000);
  QC_CHECK_EQ(core.snapshot(2105).state, CoordinatorState::PostCommandListening);
  core.on_frame(ByteView(invariant_frame(0xDF).bytes), 2165);
  QC_CHECK_EQ(core.snapshot(2165).last_transaction_outcome.value(),
              TransactionOutcome::Confirmed);
}

QC_TEST("bug_check", "deferred command keeps its immutable prior after tail contradiction") {
  auto core = ready_core();
  core.request_manual_refresh(0);
  const auto query = start_tx(core, 0);
  core.on_tx_complete(query.token, 100);
  two_frames(core, 0xDF, 300);

  const auto confirmed = std::get<ConfirmedStateAuthority>(
      core.snapshot(360).authority.state);
  core.request_state(FanState::command(Speed::High, Duration::Continuous), 400);
  const auto contradiction = invariant_frame(0xFF);
  core.on_frame(ByteView(contradiction.bytes), 500);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(
      core.snapshot(500).authority.state));

  core.poll(kResponseTailEndMs + 1);
  const auto snapshot = core.snapshot(kResponseTailEndMs + 1);
  QC_CHECK(snapshot.transaction->prior_authority.has_value());
  QC_CHECK_EQ(snapshot.transaction->prior_authority->state, confirmed.state);
  QC_CHECK_EQ(snapshot.transaction->prior_authority->revision,
              confirmed.revision);
}

QC_TEST("bug_check", "learning abandons a deferred command") {
  auto core = ready_core();
  const auto oem_query = FrameCodec::encode_query(invariant_sender());
  core.on_frame(ByteView(oem_query.bytes), 0);
  const auto request =
      FanState::command(Speed::Low, Duration::Continuous);
  core.request_state(request, 1);
  QC_CHECK(core.snapshot(1).deferred_command.has_value());

  core.request_learn(LearnMode::Manual, 2);
  QC_CHECK(!core.snapshot(2).deferred_command.has_value());
  const auto learn_frame = invariant_frame(0x9F);
  core.on_frame(ByteView(learn_frame.bytes), 3);
  core.on_frame(ByteView(learn_frame.bytes), 604);
  core.request_state(request, 605);
  QC_CHECK_EQ(core.snapshot(605).state, CoordinatorState::CommandPending);
}

QC_TEST("bug_check", "TX token exhaustion cannot install a tokenless lease state") {
  auto core = ready_core();
  ConfirmationCoreTestBuilder::set_next_tx_token(
      core, std::numeric_limits<std::uint64_t>::max());
  core.request_state(
      FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto last_lease = tx_from(core.poll(0));
  QC_CHECK(last_lease.has_value());
  QC_CHECK_EQ(last_lease->token.value(),
              std::numeric_limits<std::uint64_t>::max());

  core.request_state(
      FanState::command(Speed::High, Duration::Continuous), 1);
  core.poll(1);
  const auto snapshot = core.snapshot(1);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
  QC_CHECK(ConfirmationCore::context_matches_state(snapshot.state,
                                                   snapshot.context));
  QC_CHECK(!snapshot.live_tx.has_value());
}

QC_TEST("contexts", "all 31 enum values have a valid typed context mapping") {
  for (std::uint8_t value = 0; value < 31; ++value) {
    const auto state = static_cast<CoordinatorState>(value);
    const auto context = ConfirmationCore::context_for_test(state);
    QC_CHECK(ConfirmationCore::context_matches_state(state, context));
    const auto next = static_cast<CoordinatorState>((value + 1U) % 31U);
    QC_CHECK(!ConfirmationCore::context_matches_state(next, context) ||
             context.index() == ConfirmationCore::context_for_test(next).index());
  }
  QC_CHECK(!ConfirmationCoreTestBuilder::make(
                CoordinatorState::CommandPending, IdleContext{})
                .has_value());
}

}  // namespace
}  // namespace quietcool
