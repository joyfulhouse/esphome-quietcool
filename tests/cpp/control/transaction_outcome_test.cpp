#include "quietcool/core/confirmation_core.h"
#include "support/test.h"

#include <array>
#include <optional>

namespace quietcool {
namespace {

SenderId outcome_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

ConfirmationCore outcome_core() {
  ConfirmationCore core(CoreConfig{73});
  RestorableState restored;
  restored.sender = outcome_sender();
  core.restore(restored, 0);
  return core;
}

std::optional<TxRequest> outcome_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* request = std::get_if<RequestTxBurst>(&effects[index]))
      return request->request;
  return std::nullopt;
}

TxRequest outcome_start(ConfirmationCore& core, MonotonicMs now_ms) {
  const auto request = outcome_tx(core.poll(now_ms));
  QC_CHECK(request.has_value());
  core.on_tx_started(request->token, now_ms);
  return *request;
}

FrameBytes outcome_frame(std::uint8_t state) {
  return {{0xCB, 0x00, 0x47, 0x39, state, state}};
}

void outcome_consensus(ConfirmationCore& core, std::uint8_t state,
                       MonotonicMs first_ms) {
  const auto frame = outcome_frame(state);
  core.on_frame(ByteView(frame.bytes), first_ms);
  core.on_frame(ByteView(frame.bytes), first_ms + kMinIndependentCandidateGapMs);
}

void check_outcome(const ConfirmationCore& core, TransactionOutcome expected,
                   MonotonicMs now_ms) {
  const auto snapshot = core.snapshot(now_ms);
  QC_CHECK(snapshot.last_transaction_outcome.has_value());
  QC_CHECK_EQ(snapshot.last_transaction_outcome.value(), expected);
}

QC_TEST("transaction_outcomes", "every terminal outcome has a named core path") {
  std::array<bool, 8> reached{};
  const auto note = [&](TransactionOutcome outcome) {
    reached[static_cast<std::size_t>(outcome)] = true;
  };

  {
    auto core = outcome_core();
    core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
    const auto command = outcome_start(core, 0);
    core.on_tx_complete(command.token, 400);
    outcome_consensus(core, 0xDF, 1105);
    check_outcome(core, TransactionOutcome::Confirmed, 1165);
    note(TransactionOutcome::Confirmed);
  }
  {
    auto core = outcome_core();
    core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
    MonotonicMs attempt_ms = 0;
    for (std::uint8_t attempt = 0; attempt < 4; ++attempt) {
      const auto command = outcome_start(core, attempt_ms);
      core.on_tx_complete(command.token, attempt_ms + 400);
      outcome_consensus(core, 0xC0, attempt_ms + 1105);
      if (attempt < 3) core.poll(attempt_ms + 2901);
      attempt_ms += 2901;
    }
    check_outcome(core, TransactionOutcome::Exhausted, attempt_ms - 1736);
    note(TransactionOutcome::Exhausted);
  }
  {
    auto core = outcome_core();
    core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
    core.request_state(FanState::command(Speed::High, Duration::Continuous), 1);
    check_outcome(core, TransactionOutcome::Superseded, 1);
    note(TransactionOutcome::Superseded);
  }
  {
    auto core = outcome_core();
    core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
    const auto query = FrameCodec::encode_query(outcome_sender());
    core.on_frame(ByteView(query.bytes), 1);
    check_outcome(core, TransactionOutcome::CancelledByExactOemQuery, 1);
    note(TransactionOutcome::CancelledByExactOemQuery);
  }
  {
    auto core = outcome_core();
    core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
    const auto external = outcome_frame(0xDF);
    core.on_frame(ByteView(external.bytes), 1);
    check_outcome(core, TransactionOutcome::CancelledByExternalState, 1);
    note(TransactionOutcome::CancelledByExternalState);
  }
  {
    auto core = outcome_core();
    core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
    const auto command = outcome_start(core, 0);
    core.on_tx_complete(command.token, 400);
    outcome_consensus(core, 0xBF, 1105);
    check_outcome(core, TransactionOutcome::YieldedToPossibleOemCommand, 1165);
    note(TransactionOutcome::YieldedToPossibleOemCommand);
  }
  {
    // CancelledForLearning is unreachable by design since issue #16: a live
    // transaction implies a bound sender, and a bound sender refuses Learn
    // before it can cancel anything. The enum member is retained (it names a
    // persisted/telemetry value), and this block pins the replacement
    // behaviour: the refused Learn leaves the transaction running.
    auto core = outcome_core();
    core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
    core.request_learn(LearnMode::Manual, 1);
    const auto snapshot = core.snapshot(1);
    QC_CHECK(!snapshot.last_transaction_outcome.has_value());
    QC_CHECK(snapshot.transaction.has_value());
    QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
  }
  {
    auto core = outcome_core();
    core.request_state(FanState::command(Speed::Low, Duration::Continuous), 0);
    core.poll(0);
    core.poll(kTxLeaseStartWatchdogMs);
    core.poll(kTxLeaseStartWatchdogMs + kTxBurstWatchdogMs);
    core.poll(kTxLeaseStartWatchdogMs + 2 * kTxBurstWatchdogMs);
    check_outcome(core, TransactionOutcome::RadioUnavailable,
                  kTxLeaseStartWatchdogMs + 2 * kTxBurstWatchdogMs);
    note(TransactionOutcome::RadioUnavailable);
  }

  // CancelledForLearning is the one outcome with no core path left: since
  // issue #16 Learn is refused while provisioned, and only a provisioned core
  // can own a transaction. It stays in the enum as a persisted/telemetry name.
  for (std::size_t index = 0; index < reached.size(); ++index) {
    if (index ==
        static_cast<std::size_t>(TransactionOutcome::CancelledForLearning))
      continue;
    QC_CHECK(reached[index]);
  }
}

}  // namespace
}  // namespace quietcool
