#include "quietcool/core/confirmation_core.h"
#include "support/test.h"

#include <array>
#include <optional>
#include <variant>

namespace quietcool {
namespace {

SenderId recovery_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

std::optional<TxRequest> recovery_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* value = std::get_if<RequestTxBurst>(&effects[index]))
      return value->request;
  return std::nullopt;
}

template <typename T>
std::size_t effect_count(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < effects.size(); ++index)
    count += std::holds_alternative<T>(effects[index]);
  return count;
}

struct LeaseFixture final {
  ConfirmationCore core;
  TxRequest request;
  MonotonicMs leased_ms;
};

ConfirmationCore recovery_core() {
  ConfirmationCore core(CoreConfig{31});
  RestorableState restored;
  restored.sender = recovery_sender();
  core.restore(restored, 0);
  return core;
}

LeaseFixture command_lease() {
  auto core = recovery_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto request = *recovery_tx(core.poll(0));
  return {std::move(core), request, 0};
}

LeaseFixture boot_lease() {
  auto core = recovery_core();
  core.on_radio_ready(0);
  const auto request = *recovery_tx(core.poll(0));
  return {std::move(core), request, 0};
}

LeaseFixture manual_lease() {
  auto core = recovery_core();
  core.request_manual_refresh(0);
  const auto request = *recovery_tx(core.poll(0));
  return {std::move(core), request, 0};
}

LeaseFixture fallback_lease() {
  auto core = recovery_core();
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto command = *recovery_tx(core.poll(0));
  core.on_tx_started(command.token, 0);
  core.on_tx_complete(command.token, 400);
  core.poll(2001);
  core.poll(2901);
  const auto request = *recovery_tx(core.poll(2901));
  return {std::move(core), request, 2901};
}

LeaseFixture automatic_recovery_lease() {
  auto core = recovery_core();
  const auto query = FrameCodec::encode_query(recovery_sender());
  core.on_frame(ByteView(query.bytes), 0);
  core.poll(kOemHoldoffMs);
  core.poll(kOemRecoveryQuietMs);
  const auto request = *recovery_tx(core.poll(kOemRecoveryQuietMs));
  return {std::move(core), request, kOemRecoveryQuietMs};
}

LeaseFixture timer_recovery_lease() {
  auto core = recovery_core();
  core.request_state(FanState::command(Speed::Low, Duration::Hours1), 0);
  const auto command = *recovery_tx(core.poll(0));
  core.on_tx_started(command.token, 0);
  core.on_tx_complete(command.token, 400);
  const FrameBytes response{{0xCB, 0x00, 0x47, 0x39, 0xD1, 0xD1}};
  core.on_frame(ByteView(response.bytes), 1105);
  core.on_frame(ByteView(response.bytes), 1165);
  core.poll(2901);
  core.poll(3600400);
  const auto due = core.snapshot(3600400).recovery.due_ms;
  core.poll(due);
  const auto request = *recovery_tx(core.poll(due));
  return {std::move(core), request, due};
}

using FixtureFactory = LeaseFixture (*)();

QC_TEST("radio_recovery", "every lease watchdog revokes and preserves allowance") {
  const std::array<FixtureFactory, 5> factories{
      command_lease, boot_lease, manual_lease, fallback_lease,
      automatic_recovery_lease};
  for (std::size_t index = 0; index < factories.size(); ++index) {
    auto fixture = factories[index]();
    const auto effects = fixture.core.poll(
        fixture.leased_ms + kTxLeaseStartWatchdogMs);
    const auto snapshot = fixture.core.snapshot(
        fixture.leased_ms + kTxLeaseStartWatchdogMs);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::RadioRecovery);
    QC_CHECK_EQ(effect_count<RevokeTxLease>(effects), 1U);
    QC_CHECK_EQ(effect_count<RequestRadioReset>(effects), 1U);
    const auto context = std::get<RadioRecoveryContext>(snapshot.context);
    QC_CHECK_EQ(context.target,
                index == 0
                    ? RadioRecoveryTarget::ReissueUnstartedCommandLease
                    : RadioRecoveryTarget::ReissueUnstartedQueryLease);
    if (index == 0)
      QC_CHECK_EQ(snapshot.transaction->attempts_started, 0U);
  }
}

QC_TEST("radio_recovery", "every burst watchdog selects a started-work target") {
  const std::array<FixtureFactory, 5> factories{
      command_lease, boot_lease, manual_lease, fallback_lease,
      automatic_recovery_lease};
  for (std::size_t index = 0; index < factories.size(); ++index) {
    auto fixture = factories[index]();
    fixture.core.on_tx_started(fixture.request.token, fixture.leased_ms);
    const auto effects = fixture.core.poll(
        fixture.leased_ms + kTxBurstWatchdogMs);
    const auto snapshot = fixture.core.snapshot(
        fixture.leased_ms + kTxBurstWatchdogMs);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::RadioRecovery);
    QC_CHECK_EQ(effect_count<RequestRadioReset>(effects), 1U);
    const auto context = std::get<RadioRecoveryContext>(snapshot.context);
    QC_CHECK_EQ(context.target,
                index == 0
                    ? RadioRecoveryTarget::QuarantineStartedCommandThenRetry
                    : RadioRecoveryTarget::FinishStartedQueryAsMiss);
    if (index == 0)
      QC_CHECK_EQ(snapshot.transaction->attempts_started, 1U);
  }
}

QC_TEST("radio_recovery", "reset watchdog exhausts at the typed bound") {
  auto fixture = command_lease();
  fixture.core.poll(kTxLeaseStartWatchdogMs);
  auto effects = fixture.core.poll(kTxLeaseStartWatchdogMs +
                                   kTxBurstWatchdogMs);
  QC_CHECK_EQ(effect_count<RequestRadioReset>(effects), 1U);
  QC_CHECK_EQ(std::get<RadioRecoveryContext>(
                  fixture.core.snapshot(2000).context).attempts,
              2U);

  effects = fixture.core.poll(kTxLeaseStartWatchdogMs +
                              2 * kTxBurstWatchdogMs);
  const auto snapshot = fixture.core.snapshot(3500);
  QC_CHECK_EQ(effect_count<RequestRadioReset>(effects), 0U);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::Idle);
  QC_CHECK_EQ(snapshot.last_transaction_outcome.value(),
              TransactionOutcome::RadioUnavailable);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(
      snapshot.authority.state));
}

QC_TEST("radio_recovery", "successful reset applies every typed resume route") {
  {
    auto fixture = command_lease();
    fixture.core.poll(kTxLeaseStartWatchdogMs);
    fixture.core.on_radio_recovered(kTxLeaseStartWatchdogMs + 1);
    QC_CHECK_EQ(fixture.core.snapshot(501).state,
                CoordinatorState::CommandPending);
  }
  {
    const std::array<FixtureFactory, 5> factories{
        boot_lease, manual_lease, fallback_lease, automatic_recovery_lease,
        timer_recovery_lease};
    const std::array<CoordinatorState, 5> pending_states{
        CoordinatorState::BootQueryPending,
        CoordinatorState::ManualQueryPending,
        CoordinatorState::FallbackQueryPending,
        CoordinatorState::RecoveryQueryPending,
        CoordinatorState::RecoveryQueryPending};
    for (std::size_t index = 0; index < factories.size(); ++index) {
      auto fixture = factories[index]();
      fixture.core.poll(fixture.leased_ms + kTxLeaseStartWatchdogMs);
      fixture.core.on_radio_recovered(
          fixture.leased_ms + kTxLeaseStartWatchdogMs + 1);
      QC_CHECK_EQ(fixture.core.snapshot(
                      fixture.leased_ms + kTxLeaseStartWatchdogMs + 1).state,
                  pending_states[index]);
    }
  }
  {
    auto fixture = command_lease();
    fixture.core.on_tx_started(fixture.request.token, 0);
    fixture.core.poll(kTxBurstWatchdogMs);
    fixture.core.on_radio_recovered(kTxBurstWatchdogMs + 1);
    QC_CHECK_EQ(fixture.core.snapshot(1501).state,
                CoordinatorState::ResponseTailQuarantine);
  }
  {
    auto fixture = boot_lease();
    fixture.core.on_tx_started(fixture.request.token, 0);
    fixture.core.poll(kTxBurstWatchdogMs);
    fixture.core.on_radio_recovered(kTxBurstWatchdogMs + 1);
    QC_CHECK_EQ(fixture.core.snapshot(1501).state,
                CoordinatorState::ResponseTailQuarantine);
  }
  {
    auto fixture = boot_lease();
    fixture.core.on_tx_rejected(fixture.request.token, 1);
    QC_CHECK_EQ(std::get<RadioRecoveryContext>(
                    fixture.core.snapshot(1).context).target,
                RadioRecoveryTarget::ReturnIdleUnknown);
    fixture.core.on_radio_recovered(2);
    QC_CHECK_EQ(fixture.core.snapshot(2).state, CoordinatorState::Idle);
  }
}

QC_TEST("radio_recovery", "started query recovery applies every miss exit") {
  const std::array<FixtureFactory, 5> factories{
      boot_lease, manual_lease, fallback_lease, automatic_recovery_lease,
      timer_recovery_lease};
  for (std::size_t index = 0; index < factories.size(); ++index) {
    auto fixture = factories[index]();
    fixture.core.on_tx_started(fixture.request.token, fixture.leased_ms);
    fixture.core.poll(fixture.leased_ms + kTxBurstWatchdogMs);
    fixture.core.on_radio_recovered(
        fixture.leased_ms + kTxBurstWatchdogMs + 1);
    const auto snapshot = fixture.core.snapshot(
        fixture.leased_ms + kTxBurstWatchdogMs + 1);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::ResponseTailQuarantine);
    const auto& exit = std::get<TailQuarantineContext>(snapshot.context).exit;
    if (index == 2)
      QC_CHECK(std::holds_alternative<BeginRetryForTransaction>(exit));
    else if (index == 3)
      QC_CHECK(std::holds_alternative<BeginRecoveryRetryWait>(exit));
    else
      QC_CHECK(std::holds_alternative<ReturnIdle>(exit));
  }
}

QC_TEST("radio_recovery", "expired timer allowance is not reissued after reset") {
  auto fixture = timer_recovery_lease();
  fixture.core.poll(fixture.leased_ms + kTxLeaseStartWatchdogMs);
  const auto expires = fixture.core.snapshot(fixture.leased_ms).recovery.expires_ms;
  fixture.core.on_radio_recovered(expires + 1);
  const auto snapshot = fixture.core.snapshot(expires + 1);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::Idle);
  QC_CHECK_EQ(snapshot.logical_query_bursts, 0U);
  QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::Inactive);
}

QC_TEST("radio_recovery", "OEM and learning preempt bounded reset") {
  {
    auto fixture = command_lease();
    fixture.core.poll(kTxLeaseStartWatchdogMs);
    const auto oem = FrameCodec::encode_query(recovery_sender());
    fixture.core.on_frame(ByteView(oem.bytes), kTxLeaseStartWatchdogMs + 1);
    QC_CHECK_EQ(fixture.core.snapshot(kTxLeaseStartWatchdogMs + 1).state,
                CoordinatorState::OemHoldoff);
  }
  {
    // Issue #16: on a provisioned unit a Learn no longer preempts anything —
    // it is refused, and the bounded reset keeps running with its transaction.
    auto fixture = command_lease();
    fixture.core.poll(kTxLeaseStartWatchdogMs);
    fixture.core.request_learn(LearnMode::Manual,
                               kTxLeaseStartWatchdogMs + 1);
    const auto snapshot = fixture.core.snapshot(kTxLeaseStartWatchdogMs + 1);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::RadioRecovery);
    QC_CHECK(!snapshot.last_transaction_outcome.has_value());
    QC_CHECK(snapshot.transaction.has_value());
    QC_CHECK(!snapshot.learning.active);
  }
}

}  // namespace
}  // namespace quietcool
