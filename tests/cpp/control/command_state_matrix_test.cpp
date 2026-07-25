#include "quietcool/core/confirmation_core.h"
#include "support/core_test_builder.h"
#include "support/test.h"

#include <array>
#include <limits>
#include <optional>
#include <variant>

namespace quietcool {
namespace {

SenderId matrix_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

FanState matrix_low() {
  return FanState::command(Speed::Low, Duration::Continuous);
}

FanState matrix_high() {
  return FanState::command(Speed::High, Duration::Continuous);
}

ConfirmationCore matrix_core() {
  ConfirmationCore core(CoreConfig{101});
  RestorableState restored;
  restored.sender = matrix_sender();
  core.restore(restored, 0);
  return core;
}

std::optional<TxRequest> matrix_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* request = std::get_if<RequestTxBurst>(&effects[index]))
      return request->request;
  return std::nullopt;
}

template <typename Effect>
std::size_t matrix_effect_count(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < effects.size(); ++index)
    count += std::holds_alternative<Effect>(effects[index]);
  return count;
}

enum class QueryStage : std::uint8_t {
  Pending,
  LeaseIssued,
  Transmitting,
  ResponseListening,
};

struct QueryFixture final {
  ConfirmationCore core;
  MonotonicMs start_ms;
  MonotonicMs now_ms;
  std::optional<TxRequest> request;
};

QueryFixture query_fixture(QueryPurpose purpose, QueryStage stage) {
  auto core = matrix_core();
  MonotonicMs start_ms = 0;
  if (purpose == QueryPurpose::Boot) {
    core.on_radio_ready(0);
  } else if (purpose == QueryPurpose::Manual) {
    core.request_manual_refresh(0);
  } else if (purpose == QueryPurpose::Fallback) {
    core.request_state(matrix_low(), 0);
    const auto command = matrix_tx(core.poll(0));
    QC_CHECK(command.has_value());
    core.on_tx_started(command->token, 0);
    core.on_tx_complete(command->token, 400);
    core.poll(2001);
    core.poll(2901);
    start_ms = 2901;
  } else {
    const auto query = FrameCodec::encode_query(matrix_sender());
    core.on_frame(ByteView(query.bytes), 0);
    core.poll(kOemHoldoffMs);
    core.poll(kOemRecoveryQuietMs);
    start_ms = kOemRecoveryQuietMs;
  }

  if (stage == QueryStage::Pending)
    return {std::move(core), start_ms, start_ms, std::nullopt};

  const auto request = matrix_tx(core.poll(start_ms));
  QC_CHECK(request.has_value());
  if (stage == QueryStage::LeaseIssued)
    return {std::move(core), start_ms, start_ms, request};

  core.on_tx_started(request->token, start_ms);
  if (stage == QueryStage::Transmitting)
    return {std::move(core), start_ms, start_ms, request};

  core.on_tx_complete(request->token, start_ms + 100);
  return {std::move(core), start_ms, start_ms + 100, request};
}

void assert_replacement_started(ConfirmationCore& core, MonotonicMs now_ms) {
  const auto snapshot = core.snapshot(now_ms);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
  QC_CHECK(snapshot.transaction.has_value());
  QC_CHECK_EQ(snapshot.transaction->requested, matrix_high());
  QC_CHECK(!snapshot.deferred_command.has_value());
  QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::Inactive);
}

QC_TEST("supersession", "every query-family phase has an explicit command path") {
  const std::array<QueryPurpose, 4> purposes{
      QueryPurpose::Boot, QueryPurpose::Manual, QueryPurpose::Fallback,
      QueryPurpose::Recovery};
  const std::array<QueryStage, 4> stages{
      QueryStage::Pending, QueryStage::LeaseIssued, QueryStage::Transmitting,
      QueryStage::ResponseListening};

  for (const auto purpose : purposes) {
    for (const auto stage : stages) {
      auto fixture = query_fixture(purpose, stage);
      const auto effects =
          fixture.core.request_state(matrix_high(), fixture.now_ms + 1);

      if (stage == QueryStage::Pending) {
        assert_replacement_started(fixture.core, fixture.now_ms + 1);
      } else if (stage == QueryStage::LeaseIssued) {
        QC_CHECK_EQ(matrix_effect_count<RevokeTxLease>(effects), 1U);
        if (purpose == QueryPurpose::Fallback) {
          QC_CHECK_EQ(fixture.core.snapshot(fixture.now_ms + 1).state,
                      CoordinatorState::ResponseTailQuarantine);
          fixture.core.poll(fixture.start_ms + kResponseTailEndMs + 1);
          assert_replacement_started(
              fixture.core, fixture.start_ms + kResponseTailEndMs + 1);
        } else {
          assert_replacement_started(fixture.core, fixture.now_ms + 1);
        }
      } else {
        if (stage == QueryStage::Transmitting) {
          QC_CHECK_EQ(matrix_effect_count<RequestRadioReset>(effects), 1U);
          QC_CHECK_EQ(fixture.core.snapshot(fixture.now_ms + 1).state,
                      CoordinatorState::RadioRecovery);
          fixture.core.on_radio_recovered(fixture.now_ms + 2);
        } else {
          QC_CHECK_EQ(fixture.core.snapshot(fixture.now_ms + 1).state,
                      CoordinatorState::ResponseTailQuarantine);
        }
        fixture.core.poll(fixture.start_ms + kResponseTailEndMs + 1);
        assert_replacement_started(
            fixture.core, fixture.start_ms + kResponseTailEndMs + 1);
      }
    }
  }
}

QC_TEST("supersession", "RetryDelay duplicate joins and replacement keeps deadline") {
  auto optional = ConfirmationCoreTestBuilder::make(
      CoordinatorState::RetryDelay, RetryDelayContext{1000});
  QC_CHECK(optional.has_value());
  auto core = std::move(*optional);
  const auto original = core.snapshot(100).transaction->id;

  core.request_state(matrix_low(), 100);
  auto snapshot = core.snapshot(100);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::RetryDelay);
  QC_CHECK_EQ(snapshot.transaction->id, original);
  QC_CHECK_EQ(std::get<RetryDelayContext>(snapshot.context).due_ms, 1000U);

  core.request_state(matrix_high(), 200);
  snapshot = core.snapshot(200);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::RetryDelay);
  QC_CHECK(snapshot.transaction->id != original);
  QC_CHECK_EQ(snapshot.transaction->requested, matrix_high());
  QC_CHECK_EQ(std::get<RetryDelayContext>(snapshot.context).due_ms, 1000U);
  QC_CHECK_EQ(snapshot.last_transaction_outcome.value(),
              TransactionOutcome::Superseded);
}

QC_TEST("supersession", "RecoveryRetryWait and active radio reset yield to command") {
  {
    auto fixture = query_fixture(QueryPurpose::Recovery,
                                 QueryStage::ResponseListening);
    fixture.core.poll(fixture.start_ms + kDirectQueryAcceptEndMs + 1);
    fixture.core.poll(fixture.start_ms + kResponseTailEndMs + 1);
    QC_CHECK_EQ(fixture.core.snapshot(
                    fixture.start_ms + kResponseTailEndMs + 1).state,
                CoordinatorState::RecoveryRetryWait);
    fixture.core.request_state(
        matrix_high(), fixture.start_ms + kResponseTailEndMs + 2);
    assert_replacement_started(
        fixture.core, fixture.start_ms + kResponseTailEndMs + 2);
  }
  {
    auto core = matrix_core();
    core.request_state(matrix_low(), 0);
    QC_CHECK(matrix_tx(core.poll(0)).has_value());
    core.poll(kTxLeaseStartWatchdogMs);
    QC_CHECK_EQ(core.snapshot(kTxLeaseStartWatchdogMs).state,
                CoordinatorState::RadioRecovery);
    core.request_state(matrix_high(), kTxLeaseStartWatchdogMs + 1);
    auto snapshot = core.snapshot(kTxLeaseStartWatchdogMs + 1);
    QC_CHECK_EQ(snapshot.state, CoordinatorState::RadioRecovery);
    QC_CHECK_EQ(snapshot.deferred_command.value(), matrix_high());
    QC_CHECK_EQ(snapshot.last_transaction_outcome.value(),
                TransactionOutcome::Superseded);
    core.on_radio_recovered(kTxLeaseStartWatchdogMs + 2);
    assert_replacement_started(core, kTxLeaseStartWatchdogMs + 2);
  }
}

QC_TEST("persistence", "core refuses a replacement after transaction ID exhaustion") {
  auto core = matrix_core();
  ConfirmationCoreTestBuilder::set_next_transaction_id(
      core, std::numeric_limits<std::uint64_t>::max());
  core.request_state(matrix_low(), 0);
  const auto original = core.snapshot(0).transaction->id;
  QC_CHECK_EQ(original.value(), std::numeric_limits<std::uint64_t>::max());

  const auto effects = core.request_state(matrix_high(), 1);
  const auto snapshot = core.snapshot(1);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::CommandPending);
  QC_CHECK_EQ(snapshot.transaction->id, original);
  QC_CHECK_EQ(snapshot.transaction->requested, matrix_low());
  QC_CHECK_EQ(matrix_effect_count<RefusedInput>(effects), 1U);
  QC_CHECK_EQ(std::get<RefusedInput>(effects[0]).reason,
              RefusalReason::IdExhausted);
}

}  // namespace
}  // namespace quietcool
