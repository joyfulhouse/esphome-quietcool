#include "quietcool/core/confirmation_core.h"
#include "support/test.h"

#include <array>
#include <optional>
#include <variant>

namespace quietcool {
namespace {

SenderId passive_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

ConfirmationCore passive_core() {
  ConfirmationCore core(CoreConfig{23});
  RestorableState restored;
  restored.sender = passive_sender();
  core.restore(restored, 0);
  return core;
}

FrameBytes passive_frame(std::uint8_t value) {
  return {{0xCB, 0x00, 0x47, 0x39, value, value}};
}

std::optional<AuthoritySnapshot> published(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* effect =
            std::get_if<PublishAuthorityEffect>(&effects[index]))
      return effect->authority;
  return std::nullopt;
}

std::optional<TxRequest> requested_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* request = std::get_if<RequestTxBurst>(&effects[index]))
      return request->request;
  return std::nullopt;
}

CoreEffects passive_consensus(ConfirmationCore& core, std::uint8_t value,
                              MonotonicMs first_ms) {
  const auto frame = passive_frame(value);
  core.on_frame(ByteView(frame.bytes), first_ms);
  return core.on_frame(ByteView(frame.bytes), first_ms +
                                             kMinIndependentCandidateGapMs);
}

QC_TEST("passive_observation",
        "captured response-only burst promotes immediately") {
  auto core = passive_core();
  const auto effects = passive_consensus(core, 0x1F, 1000);
  const auto authority = published(effects);
  QC_CHECK(authority.has_value());
  const auto* confirmed =
      std::get_if<ConfirmedStateAuthority>(&authority->state);
  QC_CHECK(confirmed != nullptr);
  QC_CHECK_EQ(confirmed->source, EvidenceSource::PassiveObservationConsensus);
  QC_CHECK_EQ(confirmed->state.raw_byte(), std::uint8_t(0x1F));
  const auto snapshot = core.snapshot(1060);
  QC_CHECK_EQ(snapshot.passive_observation_epochs_opened, 0U);
  QC_CHECK_EQ(snapshot.passive_confirmations_promoted, 1U);
  QC_CHECK_EQ(snapshot.logical_command_bursts, 0U);
  QC_CHECK_EQ(snapshot.logical_query_bursts, 0U);
  QC_CHECK(!snapshot.transaction.has_value());

  // Synthetic protocol-model vectors, not claimed captures.
  auto malformed = passive_frame(0x1F);
  malformed.bytes[5] = 0x9F;
  QC_CHECK(!published(core.on_frame(ByteView(malformed.bytes), 1200)).has_value());
  auto foreign = passive_frame(0x1F);
  foreign.bytes[3] ^= 1U;
  QC_CHECK(!published(core.on_frame(ByteView(foreign.bytes), 1300)).has_value());
  const auto single = passive_frame(0x1F);
  QC_CHECK(!published(core.on_frame(ByteView(single.bytes), 1400)).has_value());
}

QC_TEST("passive_observation",
        "one ambiguous burst opens evidence window without publishing") {
  auto core = passive_core();
  const auto effects = passive_consensus(core, 0xBF, 1000);
  QC_CHECK(!published(effects).has_value());
  const auto snapshot = core.snapshot(1060);
  QC_CHECK(snapshot.passive_observation_pending);
  QC_CHECK_EQ(snapshot.passive_observation_epochs_opened, 1U);
  QC_CHECK_EQ(snapshot.passive_confirmations_promoted, 0U);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(
      snapshot.authority.state));
}

QC_TEST("passive_observation",
        "second independent captured ambiguous burst promotes in window") {
  auto core = passive_core();
  passive_consensus(core, 0xBF, 1000);
  const auto effects = passive_consensus(
      core, 0xBF, 1060 + kPassiveReplyAcceptStartMs);
  const auto authority = published(effects);
  QC_CHECK(authority.has_value());
  const auto* confirmed =
      std::get_if<ConfirmedStateAuthority>(&authority->state);
  QC_CHECK(confirmed != nullptr);
  QC_CHECK_EQ(confirmed->source, EvidenceSource::PassiveObservationConsensus);
  QC_CHECK_EQ(confirmed->independent_candidates, std::uint8_t(4));
  QC_CHECK(!core.snapshot(1520).passive_observation_pending);
}

QC_TEST("passive_observation",
        "same-burst repeats and passive window boundaries cannot promote") {
  auto core = passive_core();
  passive_consensus(core, 0xBF, 1000);
  const auto repeat = passive_frame(0xBF);
  QC_CHECK(!published(core.on_frame(ByteView(repeat.bytes), 1120)).has_value());
  QC_CHECK(!published(core.on_frame(ByteView(repeat.bytes), 1180)).has_value());
  QC_CHECK(core.snapshot(1180).passive_observation_pending);

  // Synthetic boundary vectors: exact one-millisecond misses around the named
  // inclusive evidence window, not claims about observed RF timestamps.
  auto early = passive_core();
  passive_consensus(early, 0xBF, 1000);
  QC_CHECK(!published(passive_consensus(
      early, 0xBF, 1060 + kPassiveReplyAcceptStartMs - 1)).has_value());

  auto late = passive_core();
  passive_consensus(late, 0xBF, 1000);
  QC_CHECK(!published(passive_consensus(
      late, 0xBF, 1060 + kPassiveReplyAcceptEndMs + 1)).has_value());
  QC_CHECK_EQ(late.snapshot(3000).passive_epochs_cancelled_or_expired, 1U);

  auto insufficient = passive_core();
  const auto lone = passive_frame(0xBF);
  insufficient.on_frame(ByteView(lone.bytes), 1000);
  insufficient.poll(1000 + kPassiveReplyAcceptEndMs + 1);
  insufficient.request_heartbeat(300000, 300000);
  const auto after_stale_partial = insufficient.snapshot(300000);
  QC_CHECK_EQ(after_stale_partial.heartbeat_queries_admitted, 1U);
  QC_CHECK_EQ(after_stale_partial.heartbeat_queries_skipped_busy, 0U);
}

QC_TEST("passive_observation",
        "contradiction and local ownership cancel passive evidence") {
  auto contradiction = passive_core();
  passive_consensus(contradiction, 0xBF, 1000);
  const auto other = passive_frame(0x9F);
  contradiction.on_frame(
      ByteView(other.bytes), 1060 + kPassiveReplyAcceptStartMs);
  auto snapshot = contradiction.snapshot(1460);
  QC_CHECK(!snapshot.passive_observation_pending);
  QC_CHECK_EQ(snapshot.passive_epochs_cancelled_or_expired, 1U);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(
      snapshot.authority.state));

  auto command = passive_core();
  passive_consensus(command, 0xBF, 1000);
  command.request_state(
      FanState::command(Speed::Low, Duration::Continuous), 1200);
  snapshot = command.snapshot(1200);
  QC_CHECK(!snapshot.passive_observation_pending);
  QC_CHECK(snapshot.transaction.has_value());
  QC_CHECK_EQ(snapshot.passive_epochs_cancelled_or_expired, 1U);

  auto query = passive_core();
  passive_consensus(query, 0xBF, 1000);
  query.request_manual_refresh(1200);
  snapshot = query.snapshot(1200);
  QC_CHECK(!snapshot.passive_observation_pending);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::ManualQueryPending);

  auto boot_query = passive_core();
  passive_consensus(boot_query, 0xBF, 1000);
  boot_query.on_radio_ready(1200);
  QC_CHECK(!boot_query.snapshot(1200).passive_observation_pending);

  auto recovery = passive_core();
  passive_consensus(recovery, 0xBF, 1000);
  const auto oem_query = FrameCodec::encode_query(passive_sender());
  recovery.on_frame(ByteView(oem_query.bytes), 1200);
  snapshot = recovery.snapshot(1200);
  QC_CHECK(!snapshot.passive_observation_pending);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::OemHoldoff);
}

QC_TEST("passive_observation",
        "early contradiction cancels ambiguous evidence") {
  auto core = passive_core();
  passive_consensus(core, 0xBF, 1000);
  const auto contradiction = passive_frame(0x9F);
  core.on_frame(ByteView(contradiction.bytes), 1200);

  const auto snapshot = core.snapshot(1200);
  QC_CHECK(!snapshot.passive_observation_pending);
  QC_CHECK_EQ(snapshot.passive_epochs_cancelled_or_expired, 1U);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(
      snapshot.authority.state));
}

QC_TEST("passive_observation",
        "timer expiry preserves active local response consensus") {
  auto core = passive_core();
  core.request_state(
      FanState::command(Speed::Low, Duration::Hours1), 0);
  const auto command = requested_tx(core.poll(0));
  QC_CHECK(command.has_value());
  core.on_tx_started(command->token, 0);
  core.on_tx_complete(command->token, 400);
  passive_consensus(core, 0xD1, 1105);
  core.poll(2901);
  QC_CHECK_EQ(core.snapshot(2901).state, CoordinatorState::Idle);

  constexpr MonotonicMs deadline = 3600400;
  core.request_heartbeat(3599300, 0);
  const auto query = requested_tx(core.poll(3599300));
  QC_CHECK(query.has_value());
  core.on_tx_started(query->token, 3599300);
  core.on_tx_complete(query->token, 3599300);
  const auto response = passive_frame(0xD1);
  core.on_frame(ByteView(response.bytes),
                deadline - kMinIndependentCandidateGapMs);
  core.poll(deadline);
  const auto effects = core.on_frame(ByteView(response.bytes), deadline);

  const auto authority = published(effects);
  QC_CHECK(authority.has_value());
  const auto* confirmed =
      std::get_if<ConfirmedStateAuthority>(&authority->state);
  QC_CHECK(confirmed != nullptr);
  QC_CHECK_EQ(confirmed->source, EvidenceSource::HeartbeatQueryConsensus);
  QC_CHECK_EQ(confirmed->state.raw_byte(), std::uint8_t(0xD1));
}

}  // namespace
}  // namespace quietcool
