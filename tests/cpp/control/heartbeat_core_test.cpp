#include "quietcool/core/confirmation_core.h"
#include "support/core_test_builder.h"
#include "support/test.h"

#include <optional>
#include <variant>

namespace quietcool {
namespace {

SenderId heartbeat_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

ConfirmationCore heartbeat_core() {
  ConfirmationCore core(CoreConfig{31});
  RestorableState restored;
  restored.sender = heartbeat_sender();
  core.restore(restored, 0);
  return core;
}

std::optional<TxRequest> heartbeat_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* request = std::get_if<RequestTxBurst>(&effects[index]))
      return request->request;
  return std::nullopt;
}

FrameBytes heartbeat_frame(std::uint8_t value) {
  return {{0xCB, 0x00, 0x47, 0x39, value, value}};
}

void heartbeat_consensus(ConfirmationCore& core, std::uint8_t value,
                         MonotonicMs first_ms) {
  const auto frame = heartbeat_frame(value);
  core.on_frame(ByteView(frame.bytes), first_ms);
  core.on_frame(ByteView(frame.bytes),
                first_ms + kMinIndependentCandidateGapMs);
}

TxRequest start_heartbeat(ConfirmationCore& core, MonotonicMs now_ms,
                          MonotonicMs interval_ms = 300000) {
  core.request_heartbeat(now_ms, interval_ms);
  const auto request = heartbeat_tx(core.poll(now_ms));
  QC_CHECK(request.has_value());
  core.on_tx_started(request->token, now_ms);
  return *request;
}

QC_TEST("heartbeat_core",
        "dedicated idle request emits the non-energizing OEM query") {
  auto core = heartbeat_core();
  const auto request_effects = core.request_heartbeat(1000, 300000);
  QC_CHECK_EQ(request_effects.size(), 0U);
  const auto pending = core.snapshot(1000);
  QC_CHECK_EQ(pending.state, CoordinatorState::ManualQueryPending);
  const auto* context = std::get_if<QueryPendingContext>(&pending.context);
  QC_CHECK(context != nullptr);
  QC_CHECK_EQ(context->purpose, QueryPurpose::Heartbeat);
  QC_CHECK_EQ(context->reason, TxReason::HeartbeatQuery);
  QC_CHECK_EQ(pending.heartbeat_queries_admitted, 1U);

  const auto request = heartbeat_tx(core.poll(1000));
  QC_CHECK(request.has_value());
  QC_CHECK_EQ(request->reason, TxReason::HeartbeatQuery);
  QC_CHECK_EQ(request->payload, FrameCodec::encode_query(heartbeat_sender()));
  QC_CHECK(!request->transaction.has_value());
}

QC_TEST("heartbeat_core",
        "every non-idle provisioned state skips as busy") {
  std::uint32_t visited = 0;
  for (std::uint8_t value = 0; value < kCoordinatorStateCount; ++value) {
    const auto state = static_cast<CoordinatorState>(value);
    if (state == CoordinatorState::Idle) continue;
    auto fixture = ConfirmationCoreTestBuilder::make(
        state, ConfirmationCore::context_for_test(state));
    QC_CHECK(fixture.has_value());
    const auto before = fixture->snapshot(1000);
    const auto effects = fixture->request_heartbeat(1001, 300000);
    const auto after = fixture->snapshot(1001);
    QC_CHECK_EQ(effects.size(), 0U);
    QC_CHECK_EQ(after.state, before.state);
    QC_CHECK_EQ(after.authority.revision, before.authority.revision);
    QC_CHECK_EQ(after.heartbeat_queries_skipped_busy,
                before.heartbeat_queries_skipped_busy + 1U);
    ++visited;
  }
  QC_CHECK_EQ(visited, static_cast<std::uint32_t>(kCoordinatorStateCount - 1U));
}

QC_TEST("heartbeat_core", "unprovisioned heartbeat requests stay silent") {
  ConfirmationCore core(CoreConfig{31});
  const auto before = core.snapshot(1000);
  const auto effects = core.request_heartbeat(1001, 300000);
  const auto after = core.snapshot(1001);
  QC_CHECK_EQ(effects.size(), 0U);
  QC_CHECK_EQ(after.state, CoordinatorState::Unprovisioned);
  QC_CHECK_EQ(after.heartbeat_queries_skipped_busy,
              before.heartbeat_queries_skipped_busy);
  QC_CHECK_EQ(after.heartbeat_queries_admitted,
              before.heartbeat_queries_admitted);
}

QC_TEST("heartbeat_core",
        "recent confirmed authority suppresses a redundant query") {
  auto core = heartbeat_core();
  heartbeat_consensus(core, 0x1F, 1000);
  const auto before = core.snapshot(1060);
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(
      before.authority.state));
  const auto effects = core.request_heartbeat(2000, 300000);
  const auto after = core.snapshot(2000);
  QC_CHECK_EQ(effects.size(), 0U);
  QC_CHECK_EQ(after.state, CoordinatorState::Idle);
  QC_CHECK_EQ(after.authority.revision, before.authority.revision);
  QC_CHECK_EQ(after.heartbeat_queries_suppressed_recent, 1U);
  QC_CHECK_EQ(after.heartbeat_queries_admitted, 0U);
}

QC_TEST("heartbeat_core",
        "confirmed response promotes heartbeat evidence without transaction") {
  auto core = heartbeat_core();
  const auto query = start_heartbeat(core, 1000);
  core.on_tx_complete(query.token, 1100);
  heartbeat_consensus(core, 0x1F, 1300);
  const auto snapshot = core.snapshot(1360);
  const auto* confirmed =
      std::get_if<ConfirmedStateAuthority>(&snapshot.authority.state);
  QC_CHECK(confirmed != nullptr);
  QC_CHECK_EQ(confirmed->source, EvidenceSource::HeartbeatQueryConsensus);
  QC_CHECK_EQ(confirmed->state.raw_byte(), std::uint8_t(0x1F));
  QC_CHECK(!snapshot.transaction.has_value());
  QC_CHECK_EQ(snapshot.logical_command_bursts, 0U);
  QC_CHECK_EQ(snapshot.logical_query_bursts, 1U);
  QC_CHECK_EQ(snapshot.heartbeat_misses, 0U);
}

QC_TEST("heartbeat_core",
        "timeout and invalid response preserve confirmed authority") {
  auto core = heartbeat_core();
  heartbeat_consensus(core, 0x1F, 1000);
  const auto before = core.snapshot(1060);
  const auto* before_state =
      std::get_if<ConfirmedStateAuthority>(&before.authority.state);
  QC_CHECK(before_state != nullptr);

  const auto query = start_heartbeat(core, 302000);
  core.on_tx_complete(query.token, 302100);
  auto invalid = heartbeat_frame(0x90);
  invalid.bytes[5] = 0xB0;
  core.on_frame(ByteView(invalid.bytes), 302400);
  core.poll(303101);
  const auto after = core.snapshot(303101);
  const auto* after_state =
      std::get_if<ConfirmedStateAuthority>(&after.authority.state);
  QC_CHECK(after_state != nullptr);
  QC_CHECK_EQ(after.authority.revision, before.authority.revision);
  QC_CHECK_EQ(after_state->state.raw_byte(), before_state->state.raw_byte());
  QC_CHECK_EQ(after_state->source, before_state->source);
  QC_CHECK_EQ(after_state->observed_ms, before_state->observed_ms);
  QC_CHECK_EQ(after.authority.timer.index(), before.authority.timer.index());
  QC_CHECK_EQ(after.heartbeat_misses, 1U);
}

QC_TEST("heartbeat_core",
        "radio recovery miss preserves authority and never energizes") {
  auto core = heartbeat_core();
  heartbeat_consensus(core, 0x1F, 1000);
  const auto before = core.snapshot(1060);
  const auto query = start_heartbeat(core, 302000);
  const auto fault = core.on_tx_fault(query.token, 302100);
  QC_CHECK_EQ(fault.size(), 1U);
  QC_CHECK(std::holds_alternative<RequestRadioReset>(fault[0]));
  core.on_radio_recovered(304000);
  const auto after = core.snapshot(304000);
  QC_CHECK_EQ(after.authority.revision, before.authority.revision);
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(
      after.authority.state));
  QC_CHECK(!after.transaction.has_value());
  QC_CHECK_EQ(after.logical_command_bursts, 0U);
  QC_CHECK_EQ(after.heartbeat_misses, 1U);
}

}  // namespace
}  // namespace quietcool
