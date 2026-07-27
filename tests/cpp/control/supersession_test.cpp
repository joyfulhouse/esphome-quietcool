#include "quietcool/core/confirmation_core.h"
#include "support/test.h"

#include <optional>
#include <variant>

namespace quietcool {
namespace {

SenderId supersession_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

FrameBytes supersession_frame(std::uint8_t state) {
  return {{0xCB, 0x00, 0x47, 0x39, state, state}};
}

std::optional<TxRequest> supersession_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* value = std::get_if<RequestTxBurst>(&effects[index]))
      return value->request;
  return std::nullopt;
}

template <typename T>
std::size_t supersession_effects(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < effects.size(); ++index)
    count += std::holds_alternative<T>(effects[index]);
  return count;
}

ConfirmationCore supersession_core() {
  ConfirmationCore core(CoreConfig{47});
  RestorableState restored;
  restored.sender = supersession_sender();
  core.restore(restored, 0);
  return core;
}

FanState low_on() {
  return FanState::command(Speed::Low, Duration::Continuous);
}
FanState high_on() {
  return FanState::command(Speed::High, Duration::Continuous);
}

void reach_fallback_pending(ConfirmationCore& core) {
  core.request_state(low_on(), 0);
  const auto command = *supersession_tx(core.poll(0));
  core.on_tx_started(command.token, 0);
  core.on_tx_complete(command.token, 400);
  core.poll(2001);
  core.poll(2901);
  QC_CHECK_EQ(core.snapshot(2901).state,
              CoordinatorState::FallbackQueryPending);
}

void reach_post_command(ConfirmationCore& core) {
  core.request_state(low_on(), 0);
  const auto command = *supersession_tx(core.poll(0));
  core.on_tx_started(command.token, 0);
  core.on_tx_complete(command.token, 400);
}

void reach_active_retry_tail(ConfirmationCore& core) {
  reach_post_command(core);
  const auto mismatch = supersession_frame(0xC0);
  core.on_frame(ByteView(mismatch.bytes), 1105);
  core.on_frame(ByteView(mismatch.bytes), 1165);
  QC_CHECK(core.snapshot(1165).transaction.has_value());
  QC_CHECK_EQ(core.snapshot(1165).state,
              CoordinatorState::ResponseTailQuarantine);
}

QC_TEST("supersession", "started command quiesces before deferred replacement") {
  auto core = supersession_core();
  core.request_state(low_on(), 0);
  const auto command = *supersession_tx(core.poll(0));
  core.on_tx_started(command.token, 0);

  const auto effects = core.request_state(high_on(), 100);
  auto snapshot = core.snapshot(100);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::RadioRecovery);
  QC_CHECK_EQ(snapshot.last_transaction_outcome.value(),
              TransactionOutcome::Superseded);
  QC_CHECK_EQ(supersession_effects<RequestRadioReset>(effects), 1U);
  QC_CHECK(!snapshot.live_tx.has_value());

  core.on_radio_recovered(101);
  QC_CHECK_EQ(core.snapshot(101).state,
              CoordinatorState::ResponseTailQuarantine);
  core.poll(kResponseTailEndMs + 1);
  snapshot = core.snapshot(kResponseTailEndMs + 1);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
  QC_CHECK_EQ(snapshot.transaction->requested, high_on());
}

QC_TEST("supersession", "fallback lease transmit and response preserve tail safety") {
  {
    auto core = supersession_core();
    reach_fallback_pending(core);
    const auto query = *supersession_tx(core.poll(2901));
    const auto effects = core.request_state(high_on(), 3000);
    QC_CHECK_EQ(supersession_effects<RevokeTxLease>(effects), 1U);
    QC_CHECK_EQ(core.snapshot(3000).state,
                CoordinatorState::ResponseTailQuarantine);
    core.poll(5402);
    QC_CHECK_EQ(core.snapshot(5402).transaction->requested, high_on());
    (void) query;
  }
  {
    auto core = supersession_core();
    reach_fallback_pending(core);
    const auto query = *supersession_tx(core.poll(2901));
    core.on_tx_started(query.token, 2901);
    core.request_state(high_on(), 3000);
    QC_CHECK_EQ(core.snapshot(3000).state, CoordinatorState::RadioRecovery);
    core.on_radio_recovered(3001);
    QC_CHECK_EQ(core.snapshot(3001).state,
                CoordinatorState::ResponseTailQuarantine);
    core.poll(5402);
    QC_CHECK_EQ(core.snapshot(5402).transaction->requested, high_on());
  }
  {
    auto core = supersession_core();
    reach_fallback_pending(core);
    const auto query = *supersession_tx(core.poll(2901));
    core.on_tx_started(query.token, 2901);
    core.on_tx_complete(query.token, 3000);
    core.request_state(high_on(), 3100);
    QC_CHECK_EQ(core.snapshot(3100).state,
                CoordinatorState::ResponseTailQuarantine);
    core.poll(5402);
    QC_CHECK_EQ(core.snapshot(5402).transaction->requested, high_on());
  }
}

QC_TEST("supersession", "holdoff and tail deferred slots are latest wins") {
  {
    auto core = supersession_core();
    const auto oem = FrameCodec::encode_query(supersession_sender());
    core.on_frame(ByteView(oem.bytes), 0);
    core.request_state(low_on(), 1);
    core.request_state(high_on(), 2);
    QC_CHECK_EQ(core.snapshot(2).last_transaction_outcome.value(),
                TransactionOutcome::Superseded);
    core.poll(kOemHoldoffMs);
    QC_CHECK_EQ(core.snapshot(kOemHoldoffMs).transaction->requested, high_on());
  }
  {
    auto core = supersession_core();
    core.request_manual_refresh(0);
    const auto query = *supersession_tx(core.poll(0));
    core.on_tx_started(query.token, 0);
    core.on_tx_complete(query.token, 100);
    const auto response = supersession_frame(0xDF);
    core.on_frame(ByteView(response.bytes), 300);
    core.on_frame(ByteView(response.bytes), 360);
    core.request_state(low_on(), 400);
    core.request_state(high_on(), 401);
    QC_CHECK_EQ(core.snapshot(401).last_transaction_outcome.value(),
                TransactionOutcome::Superseded);
    core.poll(kResponseTailEndMs + 1);
    QC_CHECK_EQ(core.snapshot(kResponseTailEndMs + 1).transaction->requested,
                high_on());
  }
}

QC_TEST("supersession", "query and learning states cancel safely for commands") {
  {
    auto core = supersession_core();
    core.on_radio_ready(0);
    const auto boot = *supersession_tx(core.poll(0));
    const auto effects = core.request_state(low_on(), 1);
    QC_CHECK_EQ(supersession_effects<RevokeTxLease>(effects), 1U);
    QC_CHECK_EQ(core.snapshot(1).state, CoordinatorState::CommandPending);
    (void) boot;
  }
  {
    auto core = supersession_core();
    core.on_radio_ready(0);
    const auto boot = *supersession_tx(core.poll(0));
    core.on_tx_started(boot.token, 0);
    core.request_state(low_on(), 1);
    QC_CHECK_EQ(core.snapshot(1).state, CoordinatorState::RadioRecovery);
    core.on_radio_recovered(2);
    QC_CHECK_EQ(core.snapshot(2).state,
                CoordinatorState::ResponseTailQuarantine);
  }
  {
    // Issue #16: on a provisioned core the Learn is refused outright, so no
    // learn window ever competes with the command.
    auto core = supersession_core();
    const auto refused = core.request_learn(LearnMode::Manual, 0);
    QC_CHECK_EQ(supersession_effects<RefusedInput>(refused), 1U);
    QC_CHECK_EQ(std::get<RefusedInput>(refused[0]).reason,
                RefusalReason::AlreadyProvisioned);
    core.request_state(low_on(), 1);
    const auto snapshot = core.snapshot(1);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
    QC_CHECK(!snapshot.learning.active);
  }
  {
    ConfirmationCore core(CoreConfig{47});
    core.request_learn(LearnMode::Manual, 0);
    core.request_state(low_on(), 1);
    QC_CHECK_EQ(core.snapshot(1).state,
                CoordinatorState::LearningAwaitingFirst);
  }
}

QC_TEST("supersession", "semantic duplicates join in every active command phase") {
  auto duplicate = FanState::command(Speed::High, Duration::Off);
  auto requested = FanState::command(Speed::Low, Duration::Off);
  auto core = supersession_core();
  core.request_state(requested, 0);
  core.request_state(duplicate, 1);
  QC_CHECK_EQ(core.snapshot(1).state, CoordinatorState::CommandPending);
  const auto command = *supersession_tx(core.poll(2));
  core.request_state(duplicate, 3);
  QC_CHECK_EQ(core.snapshot(3).state, CoordinatorState::CommandLeaseIssued);
  core.on_tx_started(command.token, 4);
  core.request_state(duplicate, 5);
  QC_CHECK_EQ(core.snapshot(5).state, CoordinatorState::CommandTransmitting);
  core.on_tx_complete(command.token, 400);
  core.request_state(duplicate, 401);
  QC_CHECK_EQ(core.snapshot(401).state, CoordinatorState::PostCommandListening);
  QC_CHECK(!core.snapshot(401).last_transaction_outcome.has_value());
}

QC_TEST("supersession", "pending and unstarted lease replacements are immediate") {
  {
    auto core = supersession_core();
    core.request_state(low_on(), 0);
    core.request_state(high_on(), 1);
    const auto snapshot = core.snapshot(1);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
    QC_CHECK_EQ(snapshot.last_transaction_outcome.value(),
                TransactionOutcome::Superseded);
    QC_CHECK_EQ(snapshot.transaction->requested, high_on());
  }
  {
    auto core = supersession_core();
    core.request_state(low_on(), 0);
    const auto lease = *supersession_tx(core.poll(0));
    const auto effects = core.request_state(high_on(), 1);
    const auto snapshot = core.snapshot(1);
    QC_CHECK_EQ(supersession_effects<RevokeTxLease>(effects), 1U);
    QC_CHECK_EQ(std::get<RevokeTxLease>(effects[0]).token, lease.token);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
    QC_CHECK_EQ(snapshot.transaction->requested, high_on());
  }
}

QC_TEST("supersession", "post-command states preserve the old RF tail") {
  {
    auto core = supersession_core();
    reach_post_command(core);
    core.request_state(high_on(), 500);
    QC_CHECK_EQ(core.snapshot(500).state,
                CoordinatorState::ResponseTailQuarantine);
    QC_CHECK_EQ(core.snapshot(500).last_transaction_outcome.value(),
                TransactionOutcome::Superseded);
    core.poll(2901);
    QC_CHECK_EQ(core.snapshot(2901).transaction->requested, high_on());
  }
  {
    auto core = supersession_core();
    reach_post_command(core);
    core.poll(2001);
    QC_CHECK_EQ(core.snapshot(2001).state,
                CoordinatorState::PostCommandTailWait);
    core.request_state(high_on(), 2100);
    QC_CHECK_EQ(core.snapshot(2100).state,
                CoordinatorState::ResponseTailQuarantine);
    core.poll(2901);
    QC_CHECK_EQ(core.snapshot(2901).transaction->requested, high_on());
  }
  {
    auto core = supersession_core();
    reach_fallback_pending(core);
    core.request_state(high_on(), 2902);
    const auto snapshot = core.snapshot(2902);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
    QC_CHECK_EQ(snapshot.transaction->requested, high_on());
    QC_CHECK_EQ(snapshot.last_transaction_outcome.value(),
                TransactionOutcome::Superseded);
  }
}

QC_TEST("supersession", "active retry tail is superseded before deferral") {
  auto core = supersession_core();
  reach_active_retry_tail(core);
  core.request_state(high_on(), 1200);
  const auto snapshot = core.snapshot(1200);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::ResponseTailQuarantine);
  QC_CHECK(!snapshot.transaction.has_value());
  QC_CHECK_EQ(snapshot.deferred_command.value(), high_on());
  QC_CHECK_EQ(snapshot.last_transaction_outcome.value(),
              TransactionOutcome::Superseded);
  core.poll(2901);
  QC_CHECK_EQ(core.snapshot(2901).transaction->requested, high_on());
}

QC_TEST("supersession", "pending query and recovery work yields to a command") {
  {
    auto core = supersession_core();
    core.on_radio_ready(0);
    QC_CHECK_EQ(core.snapshot(0).state, CoordinatorState::BootQueryPending);
    core.request_state(low_on(), 1);
    QC_CHECK_EQ(core.snapshot(1).state, CoordinatorState::CommandPending);
  }
  {
    auto core = supersession_core();
    core.request_manual_refresh(0);
    QC_CHECK_EQ(core.snapshot(0).state, CoordinatorState::ManualQueryPending);
    core.request_state(low_on(), 1);
    QC_CHECK_EQ(core.snapshot(1).state, CoordinatorState::CommandPending);
  }
  {
    auto core = supersession_core();
    const auto oem = FrameCodec::encode_query(supersession_sender());
    core.on_frame(ByteView(oem.bytes), 0);
    core.poll(kOemHoldoffMs);
    QC_CHECK_EQ(core.snapshot(kOemHoldoffMs).state,
                CoordinatorState::RecoveryQuietWait);
    core.request_state(low_on(), kOemHoldoffMs + 1);
    const auto snapshot = core.snapshot(kOemHoldoffMs + 1);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
    QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::Inactive);
  }
  {
    auto core = supersession_core();
    const auto oem = FrameCodec::encode_query(supersession_sender());
    core.on_frame(ByteView(oem.bytes), 0);
    core.poll(kOemHoldoffMs);
    core.poll(kOemRecoveryQuietMs);
    QC_CHECK_EQ(core.snapshot(kOemRecoveryQuietMs).state,
                CoordinatorState::RecoveryQueryPending);
    core.request_state(low_on(), kOemRecoveryQuietMs + 1);
    const auto snapshot = core.snapshot(kOemRecoveryQuietMs + 1);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
    QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::Inactive);
  }
}

QC_TEST("supersession", "both learning phases obey provisioning") {
  {
    // Issue #16: a provisioned core never reaches a learning phase — the Learn
    // is refused, its frames stay ordinary traffic, and commands are untouched.
    auto core = supersession_core();
    const auto refused = core.request_learn(LearnMode::Manual, 0);
    QC_CHECK_EQ(supersession_effects<RefusedInput>(refused), 1U);
    QC_CHECK_EQ(std::get<RefusedInput>(refused[0]).reason,
                RefusalReason::AlreadyProvisioned);
    QC_CHECK_EQ(core.snapshot(0).state, CoordinatorState::Idle);
    core.request_state(low_on(), 2);
    QC_CHECK_EQ(core.snapshot(2).state, CoordinatorState::CommandPending);
    QC_CHECK(!core.snapshot(2).learning.active);
  }
  {
    ConfirmationCore core(CoreConfig{47});
    core.request_learn(LearnMode::Manual, 0);
    const auto candidate = supersession_frame(0x9F);
    core.on_frame(ByteView(candidate.bytes), 1);
    QC_CHECK_EQ(core.snapshot(1).state,
                CoordinatorState::LearningAwaitingSecond);
    const auto effects = core.request_state(low_on(), 2);
    QC_CHECK_EQ(core.snapshot(2).state,
                CoordinatorState::LearningAwaitingSecond);
    QC_CHECK_EQ(supersession_effects<RefusedInput>(effects), 1U);
    QC_CHECK_EQ(std::get<RefusedInput>(effects[0]).reason,
                RefusalReason::Unprovisioned);
  }
}

}  // namespace
}  // namespace quietcool
