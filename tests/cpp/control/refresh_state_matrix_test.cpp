#include "quietcool/core/confirmation_core.h"
#include "support/core_test_builder.h"
#include "support/test.h"

#include <optional>
#include <utility>
#include <variant>

namespace quietcool {
namespace {

SenderId refresh_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

ConfirmationCore refresh_core() {
  ConfirmationCore core(CoreConfig{97});
  RestorableState restored;
  restored.sender = refresh_sender();
  core.restore(restored, 0);
  return core;
}

std::optional<TxRequest> refresh_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* request = std::get_if<RequestTxBurst>(&effects[index]))
      return request->request;
  return std::nullopt;
}

std::size_t refresh_tx_count(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < effects.size(); ++index)
    count += std::holds_alternative<RequestTxBurst>(effects[index]);
  return count;
}

std::optional<RefusalReason> refresh_refusal(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* refused = std::get_if<RefusedInput>(&effects[index]))
      return refused->reason;
  return std::nullopt;
}

FrameBytes refresh_frame(std::uint8_t state) {
  return {{0xCB, 0x00, 0x47, 0x39, state, state}};
}

TxRequest start(ConfirmationCore& core, MonotonicMs now_ms) {
  const auto request = refresh_tx(core.poll(now_ms));
  QC_CHECK(request.has_value());
  core.on_tx_started(request->token, now_ms);
  return *request;
}

void reach_post_command(ConfirmationCore& core) {
  core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
  const auto command = start(core, 0);
  core.on_tx_complete(command.token, 400);
}

void reach_fallback_pending(ConfirmationCore& core) {
  reach_post_command(core);
  core.poll(2001);
  core.poll(2901);
}

void reach_recovery_pending(ConfirmationCore& core) {
  const auto oem = FrameCodec::encode_query(refresh_sender());
  core.on_frame(ByteView(oem.bytes), 0);
  core.poll(kOemHoldoffMs);
  core.poll(kOemRecoveryQuietMs);
}

struct RefreshFixture final {
  ConfirmationCore core;
  MonotonicMs now_ms;
  bool reachable;
};

RefreshFixture fixture_for(CoordinatorState state) {
  if (state == CoordinatorState::Unprovisioned)
    return {ConfirmationCore(CoreConfig{97}), 0, true};
  auto core = refresh_core();
  MonotonicMs now_ms = 0;
  switch (state) {
    case CoordinatorState::Idle:break;
    case CoordinatorState::CommandPending:
      core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
      break;
    case CoordinatorState::CommandLeaseIssued:
      core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
      core.poll(0); break;
    case CoordinatorState::CommandTransmitting:
      core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
      start(core, 0); break;
    case CoordinatorState::PostCommandListening:
      reach_post_command(core); now_ms=400; break;
    case CoordinatorState::PostCommandTailWait:
      reach_post_command(core); core.poll(2001); now_ms=2001; break;
    case CoordinatorState::FallbackQueryPending:
      reach_fallback_pending(core); now_ms=2901; break;
    case CoordinatorState::FallbackQueryLeaseIssued:
      reach_fallback_pending(core); core.poll(2901); now_ms=2901; break;
    case CoordinatorState::FallbackQueryTransmitting: {
      reach_fallback_pending(core); start(core,2901); now_ms=2901; break;
    }
    case CoordinatorState::FallbackResponseListening: {
      reach_fallback_pending(core); const auto query=start(core,2901);
      core.on_tx_complete(query.token,3001); now_ms=3001; break;
    }
    case CoordinatorState::RetryDelay:
      return {std::move(*ConfirmationCoreTestBuilder::make(
                  state, RetryDelayContext{1000})), 0, true};
    case CoordinatorState::BootQueryPending:
      core.on_radio_ready(0); break;
    case CoordinatorState::BootQueryLeaseIssued:
      core.on_radio_ready(0); core.poll(0); break;
    case CoordinatorState::BootQueryTransmitting:
      core.on_radio_ready(0); start(core,0); break;
    case CoordinatorState::BootResponseListening: {
      core.on_radio_ready(0); const auto query=start(core,0);
      core.on_tx_complete(query.token,100); now_ms=100; break;
    }
    case CoordinatorState::ManualQueryPending:
      core.request_manual_refresh(0); break;
    case CoordinatorState::ManualQueryLeaseIssued:
      core.request_manual_refresh(0); core.poll(0); break;
    case CoordinatorState::ManualQueryTransmitting:
      core.request_manual_refresh(0); start(core,0); break;
    case CoordinatorState::ManualResponseListening: {
      core.request_manual_refresh(0); const auto query=start(core,0);
      core.on_tx_complete(query.token,100); now_ms=100; break;
    }
    case CoordinatorState::OemHoldoff: {
      const auto oem=FrameCodec::encode_query(refresh_sender());
      core.on_frame(ByteView(oem.bytes),0); break;
    }
    case CoordinatorState::RecoveryQuietWait: {
      const auto oem=FrameCodec::encode_query(refresh_sender());
      core.on_frame(ByteView(oem.bytes),0); core.poll(kOemHoldoffMs);
      now_ms=kOemHoldoffMs; break;
    }
    case CoordinatorState::RecoveryQueryPending:
      reach_recovery_pending(core); now_ms=kOemRecoveryQuietMs; break;
    case CoordinatorState::RecoveryQueryLeaseIssued:
      reach_recovery_pending(core); core.poll(kOemRecoveryQuietMs);
      now_ms=kOemRecoveryQuietMs; break;
    case CoordinatorState::RecoveryQueryTransmitting:
      reach_recovery_pending(core); start(core,kOemRecoveryQuietMs);
      now_ms=kOemRecoveryQuietMs; break;
    case CoordinatorState::RecoveryResponseListening: {
      reach_recovery_pending(core); const auto query=start(core,kOemRecoveryQuietMs);
      core.on_tx_complete(query.token,kOemRecoveryQuietMs+100);
      now_ms=kOemRecoveryQuietMs+100; break;
    }
    case CoordinatorState::RecoveryRetryWait: {
      reach_recovery_pending(core); const auto query=start(core,kOemRecoveryQuietMs);
      core.on_tx_complete(query.token,kOemRecoveryQuietMs+100);
      core.poll(kOemRecoveryQuietMs+kDirectQueryAcceptEndMs+1);
      core.poll(kOemRecoveryQuietMs+kResponseTailEndMs+1);
      now_ms=kOemRecoveryQuietMs+kResponseTailEndMs+1; break;
    }
    case CoordinatorState::ResponseTailQuarantine: {
      core.request_manual_refresh(0); const auto query=start(core,0);
      core.on_tx_complete(query.token,100); const auto frame=refresh_frame(0xDF);
      core.on_frame(ByteView(frame.bytes),300);
      core.on_frame(ByteView(frame.bytes),360); now_ms=360; break;
    }
    // Since issue #16 learning states are only reachable while unprovisioned
    // (request_learn refuses when a sender is bound), so these two fixtures
    // start from a fresh core instead of the provisioned one.
    case CoordinatorState::LearningAwaitingFirst: {
      ConfirmationCore fresh(CoreConfig{97});
      fresh.request_learn(LearnMode::Manual,0);
      QC_CHECK_EQ(fresh.snapshot(0).state,state);
      return {std::move(fresh),0,true};
    }
    case CoordinatorState::LearningAwaitingSecond: {
      ConfirmationCore fresh(CoreConfig{97});
      fresh.request_learn(LearnMode::Manual,0);
      const auto frame=refresh_frame(0x9F);
      fresh.on_frame(ByteView(frame.bytes),1);
      QC_CHECK_EQ(fresh.snapshot(1).state,state);
      return {std::move(fresh),1,true};
    }
    case CoordinatorState::RadioRecovery:
      core.request_state(FanState::command(Speed::Low,Duration::Continuous),0);
      core.poll(0); core.poll(kTxLeaseStartWatchdogMs);
      now_ms=kTxLeaseStartWatchdogMs; break;
    case CoordinatorState::Unprovisioned:break;
  }
  QC_CHECK_EQ(core.snapshot(now_ms).state,state);
  return {std::move(core),now_ms,true};
}

QC_TEST("INV-09", "Refresh is inert in every reachable non-Idle state") {
  std::size_t invoked = 0;
  for (std::uint8_t value=0; value<kCoordinatorStateCount; ++value) {
    const auto state=static_cast<CoordinatorState>(value);
    if (state==CoordinatorState::Idle) continue;
    QC_CHECK(!TransitionTable::refresh_is_accepted(state));
    auto fixture=fixture_for(state);
    QC_CHECK(fixture.reachable);
    const auto before=fixture.core.snapshot(fixture.now_ms);
    const auto effects=fixture.core.request_manual_refresh(fixture.now_ms+1);
    const auto after=fixture.core.snapshot(fixture.now_ms+1);
    QC_CHECK_EQ(refresh_tx_count(effects),0U);
    QC_CHECK_EQ(after.state,before.state);
    QC_CHECK_EQ(after.authority.revision,before.authority.revision);
    QC_CHECK_EQ(after.live_tx.has_value(),before.live_tx.has_value());
    if (before.live_tx) {
      QC_CHECK_EQ(after.live_tx->token,before.live_tx->token);
      QC_CHECK_EQ(after.live_tx->reason,before.live_tx->reason);
      QC_CHECK_EQ(after.live_tx->payload,before.live_tx->payload);
    }
    QC_CHECK_EQ(after.recovery.phase,before.recovery.phase);
    QC_CHECK_EQ(after.recovery.due_ms,before.recovery.due_ms);
    QC_CHECK_EQ(after.learning.active,before.learning.active);
    QC_CHECK(refresh_refusal(effects).has_value());
    // Learning states refuse as Unprovisioned, not Learning: since issue #16 a
    // learn window only ever exists on an unprovisioned core, and the refresh
    // handler checks the missing sender first. RefusalReason::Learning is
    // retained in the enum but is no longer reachable via the public API.
    const auto expected=(state==CoordinatorState::Unprovisioned ||
                         state==CoordinatorState::LearningAwaitingFirst ||
                         state==CoordinatorState::LearningAwaitingSecond)
        ? RefusalReason::Unprovisioned
        : state==CoordinatorState::OemHoldoff ? RefusalReason::Holdoff
                                              : RefusalReason::Busy;
    QC_CHECK_EQ(*refresh_refusal(effects),expected);
    ++invoked;
  }
  QC_CHECK_EQ(invoked,30U);

  const auto rules=TransitionTable::rules();
  bool retry_delay_row=false;
  for (std::size_t index=0; index<rules.size; ++index) {
    const auto& rule=rules.data[index];
    if (rule.state==CoordinatorState::RetryDelay &&
        rule.event==EventKind::ManualRefreshRequested) {
      QC_CHECK_EQ(rule.action,ActionId::RefuseRefresh);
      QC_CHECK_EQ(rule.next,NextStateId::Same);
      retry_delay_row=true;
    }
  }
  QC_CHECK(retry_delay_row);
}

}  // namespace
}  // namespace quietcool
