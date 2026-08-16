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
        "same-burst repeats cannot promote and partial expiry recovers") {
  auto core = passive_core();
  passive_consensus(core, 0xBF, 1000);
  const auto repeat = passive_frame(0xBF);
  QC_CHECK(!published(core.on_frame(ByteView(repeat.bytes), 1120)).has_value());
  QC_CHECK(!published(core.on_frame(ByteView(repeat.bytes), 1180)).has_value());
  QC_CHECK(core.snapshot(1180).passive_observation_pending);

  auto insufficient = passive_core();
  const auto lone = passive_frame(0xBF);
  insufficient.on_frame(ByteView(lone.bytes), 1000);
  const auto expiry =
      insufficient.poll(1000 + kPassiveReplyAcceptEndMs + 1);
  const auto authority = published(expiry);
  QC_CHECK(authority.has_value());
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(authority->state));
  insufficient.request_heartbeat(300000, 300000);
  const auto after_stale_partial = insufficient.snapshot(300000);
  QC_CHECK_EQ(after_stale_partial.heartbeat_queries_admitted, 0U);
  QC_CHECK_EQ(after_stale_partial.heartbeat_queries_skipped_busy, 1U);
}

QC_TEST("passive_observation", "reply-window end is inclusive") {
  // Synthetic protocol-model timestamps, not claimed RF captures. The first
  // ambiguous consensus opens at 1060 ms.
  auto end_included = passive_core();
  passive_consensus(end_included, 0xBF, 1000);
  const auto frame = passive_frame(0xBF);
  end_included.on_frame(ByteView(frame.bytes), 2600);
  QC_CHECK(published(end_included.on_frame(ByteView(frame.bytes), 2660))
               .has_value());

  auto end_excluded = passive_core();
  passive_consensus(end_excluded, 0xBF, 1000);
  end_excluded.on_frame(ByteView(frame.bytes), 2601);
  const auto excluded =
      published(end_excluded.on_frame(ByteView(frame.bytes), 2661));
  QC_CHECK(excluded.has_value());
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(excluded->state));
}

QC_TEST("passive_observation", "inter-burst silence is inclusive") {
  const auto frame = passive_frame(0xBF);
  auto silence_included = passive_core();
  passive_consensus(silence_included, 0xBF, 1000);
  silence_included.on_frame(ByteView(frame.bytes), 1160);
  silence_included.on_frame(ByteView(frame.bytes), 1460);
  QC_CHECK(published(silence_included.on_frame(ByteView(frame.bytes), 1520))
               .has_value());

  auto silence_excluded = passive_core();
  passive_consensus(silence_excluded, 0xBF, 1000);
  silence_excluded.on_frame(ByteView(frame.bytes), 1161);
  silence_excluded.on_frame(ByteView(frame.bytes), 1460);
  QC_CHECK(!published(silence_excluded.on_frame(ByteView(frame.bytes), 1520))
                .has_value());
}

QC_TEST("passive_observation",
        "held ambiguous repeat train cannot become a second burst") {
  auto core = passive_core();
  passive_consensus(core, 0xBF, 1000);
  const auto repeat = passive_frame(0xBF);
  for (const MonotonicMs repeat_ms : {1160, 1260, 1360, 1460, 1520, 1580})
    QC_CHECK(
        !published(core.on_frame(ByteView(repeat.bytes), repeat_ms)).has_value());

  const auto snapshot = core.snapshot(1580);
  QC_CHECK(snapshot.passive_observation_pending);
  QC_CHECK_EQ(snapshot.passive_confirmations_promoted, 0U);
  QC_CHECK(std::holds_alternative<UnknownStateAuthority>(
      snapshot.authority.state));
}

QC_TEST("passive_observation",
        "response-only consensus outranks a pending ambiguous epoch") {
  auto core = passive_core();
  passive_consensus(core, 0xBF, 1000);
  const auto decisive = passive_frame(0x1F);
  core.on_frame(ByteView(decisive.bytes), 1200);
  const auto effects = core.on_frame(
      ByteView(decisive.bytes), 1200 + kMinIndependentCandidateGapMs);

  const auto authority = published(effects);
  QC_CHECK(authority.has_value());
  const auto* confirmed =
      std::get_if<ConfirmedStateAuthority>(&authority->state);
  QC_CHECK(confirmed != nullptr);
  QC_CHECK_EQ(confirmed->state.raw_byte(), std::uint8_t(0x1F));
  QC_CHECK_EQ(confirmed->source, EvidenceSource::PassiveObservationConsensus);
  QC_CHECK(!core.snapshot(1260).passive_observation_pending);
}

QC_TEST("passive_observation",
        "incomplete response-only evidence preserves ambiguous recovery") {
  auto core = passive_core();
  passive_consensus(core, 0x1F, 1000);
  passive_consensus(core, 0xBF, 2000);
  const auto response_only = passive_frame(0x1F);
  const auto first = core.on_frame(ByteView(response_only.bytes), 2200);
  QC_CHECK(!published(first).has_value());

  auto snapshot = core.snapshot(2200);
  QC_CHECK(snapshot.passive_observation_pending);
  QC_CHECK_EQ(snapshot.recovery.cause,
              std::optional<RecoveryCause>(RecoveryCause::OemActivity));
  QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::QuietWait);

  const auto expiry = core.poll(2060 + kPassiveReplyAcceptEndMs + 1);
  const auto authority = published(expiry);
  QC_CHECK(authority.has_value());
  const auto* unknown =
      std::get_if<UnknownStateAuthority>(&authority->state);
  QC_CHECK(unknown != nullptr);
  QC_CHECK_EQ(unknown->reason, AuthorityLossReason::ExternalStateTraffic);
  snapshot = core.snapshot(3661);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::RecoveryQuietWait);
}

QC_TEST("passive_observation",
        "contradiction abandons while local ownership cancels evidence") {
  auto contradiction = passive_core();
  passive_consensus(contradiction, 0x1F, 1000);
  passive_consensus(contradiction, 0xBF, 2000);
  const auto other = passive_frame(0x9F);
  const auto conflict =
      contradiction.on_frame(ByteView(other.bytes), 2200);
  const auto conflict_authority = published(conflict);
  QC_CHECK(conflict_authority.has_value());
  const auto* unknown =
      std::get_if<UnknownStateAuthority>(&conflict_authority->state);
  QC_CHECK(unknown != nullptr);
  QC_CHECK_EQ(unknown->reason, AuthorityLossReason::ExternalStateTraffic);
  auto snapshot = contradiction.snapshot(2200);
  QC_CHECK(!snapshot.passive_observation_pending);
  QC_CHECK_EQ(snapshot.passive_epochs_cancelled_or_expired, 1U);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::RecoveryQuietWait);
  QC_CHECK_EQ(snapshot.recovery.cause,
              std::optional<RecoveryCause>(RecoveryCause::OemActivity));

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
        "post-setup radio readiness publishes abandoned authority") {
  auto core = passive_core();
  core.on_radio_ready(0);
  const auto boot_query = requested_tx(core.poll(0));
  QC_CHECK(boot_query.has_value());
  core.on_tx_started(boot_query->token, 0);
  core.on_tx_complete(boot_query->token, 0);
  core.poll(kDirectQueryAcceptEndMs + 1);
  core.poll(kResponseTailEndMs + 1);
  QC_CHECK_EQ(core.snapshot(kResponseTailEndMs + 1).state,
              CoordinatorState::Idle);

  passive_consensus(core, 0x1F, 2700);
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(
      core.snapshot(2760).authority.state));
  passive_consensus(core, 0xBF, 3000);
  QC_CHECK(core.snapshot(3060).passive_observation_pending);
  const auto effects = core.on_radio_ready(3200);
  const auto snapshot = core.snapshot(3200);
  const auto authority = published(effects);
  QC_CHECK(authority.has_value());
  QC_CHECK(!snapshot.passive_observation_pending);
  QC_CHECK_EQ(snapshot.passive_epochs_cancelled_or_expired, 1U);
  const auto* unknown =
      std::get_if<UnknownStateAuthority>(&authority->state);
  QC_CHECK(unknown != nullptr);
  QC_CHECK_EQ(unknown->reason, AuthorityLossReason::ExternalStateTraffic);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::RecoveryQuietWait);
  QC_CHECK_EQ(snapshot.recovery.cause,
              std::optional<RecoveryCause>(RecoveryCause::OemActivity));
}

QC_TEST("passive_observation",
        "ambiguous epoch outcomes isolate provisional recovery") {
  auto promoted = passive_core();
  passive_consensus(promoted, 0xBF, 1000);
  const auto promotion = passive_consensus(
      promoted, 0xBF, 1060 + kPassiveReplyAcceptStartMs);
  QC_CHECK(published(promotion).has_value());
  auto snapshot = promoted.snapshot(1520);
  QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::Inactive);
  QC_CHECK(!snapshot.recovery.cause.has_value());

  auto local = passive_core();
  passive_consensus(local, 0xBF, 1000);
  local.request_state(
      FanState::command(Speed::Low, Duration::Continuous), 1200);
  snapshot = local.snapshot(1200);
  QC_CHECK(!snapshot.passive_observation_pending);
  QC_CHECK(snapshot.transaction.has_value());
  QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::Inactive);
  QC_CHECK(!snapshot.recovery.cause.has_value());

  auto missed = passive_core();
  passive_consensus(missed, 0x1F, 1000);
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(
      missed.snapshot(1060).authority.state));
  passive_consensus(missed, 0xBF, 2000);
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(
      missed.snapshot(2060).authority.state));
  const auto missed_expiry =
      missed.poll(2060 + kPassiveReplyAcceptEndMs + 1);
  const auto missed_authority = published(missed_expiry);
  QC_CHECK(missed_authority.has_value());
  snapshot = missed.snapshot(3661);
  const auto* unknown =
      std::get_if<UnknownStateAuthority>(&missed_authority->state);
  QC_CHECK(unknown != nullptr);
  QC_CHECK_EQ(unknown->reason, AuthorityLossReason::ExternalStateTraffic);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::RecoveryQuietWait);
  QC_CHECK_EQ(snapshot.recovery.cause,
              std::optional<RecoveryCause>(RecoveryCause::OemActivity));
  QC_CHECK_EQ(snapshot.recovery.phase, RecoveryPhase::QuietWait);
}

QC_TEST("passive_observation",
        "single ambiguous candidate expires but invalid noise does not") {
  auto ambiguous = passive_core();
  passive_consensus(ambiguous, 0x1F, 1000);
  const auto command_shaped = passive_frame(0xBF);
  ambiguous.on_frame(ByteView(command_shaped.bytes), 2000);
  const auto expiry = ambiguous.poll(2000 + kPassiveReplyAcceptEndMs + 1);
  const auto authority = published(expiry);
  QC_CHECK(authority.has_value());
  const auto* unknown =
      std::get_if<UnknownStateAuthority>(&authority->state);
  QC_CHECK(unknown != nullptr);
  QC_CHECK_EQ(unknown->reason, AuthorityLossReason::ExternalStateTraffic);
  const auto snapshot = ambiguous.snapshot(3601);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::RecoveryQuietWait);
  QC_CHECK_EQ(snapshot.recovery.cause,
              std::optional<RecoveryCause>(RecoveryCause::OemActivity));

  auto malformed = passive_core();
  passive_consensus(malformed, 0x1F, 1000);
  auto malformed_frame = passive_frame(0xBF);
  malformed_frame.bytes[5] = 0x9F;
  QC_CHECK(!published(malformed.on_frame(ByteView(malformed_frame.bytes), 2000))
                .has_value());
  QC_CHECK(!published(malformed.poll(3601)).has_value());
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(
      malformed.snapshot(3601).authority.state));

  auto foreign = passive_core();
  passive_consensus(foreign, 0x1F, 1000);
  auto foreign_frame = passive_frame(0xBF);
  foreign_frame.bytes[3] ^= 1U;
  QC_CHECK(!published(foreign.on_frame(ByteView(foreign_frame.bytes), 2000))
                .has_value());
  QC_CHECK(!published(foreign.poll(3601)).has_value());
  QC_CHECK(std::holds_alternative<ConfirmedStateAuthority>(
      foreign.snapshot(3601).authority.state));
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
