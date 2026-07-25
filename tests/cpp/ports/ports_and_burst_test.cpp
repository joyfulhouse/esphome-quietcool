#include "quietcool/core/confirmation_core.h"
#include "quietcool/ports/core_callback_queue.h"
#include "quietcool/radio/burst_transmitter.h"
#include "support/test.h"
#include "support/test_doubles.h"

#include <array>
#include <optional>
#include <variant>

namespace quietcool {
namespace {

static_assert(sizeof(CoreEffects) <= 800,
              "CoreEffects must remain small enough for the ESP32 loop stack");

SenderId port_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

TxRequest port_request(std::uint64_t token = 1) {
  return {TxToken(token), FrameCodec::encode_query(port_sender()),
          TxReason::ManualQuery, std::nullopt, std::nullopt, 0};
}

void forward(ConfirmationCore& core, const BurstEvent& event,
             MonotonicMs now_ms) {
  if (const auto* started = std::get_if<BurstStarted>(&event))
    core.on_tx_started(started->token, now_ms);
  else if (const auto* complete = std::get_if<BurstComplete>(&event))
    core.on_tx_complete(complete->token, now_ms);
  else if (const auto* rejected = std::get_if<BurstRejected>(&event))
    core.on_tx_rejected(rejected->token, now_ms);
}

std::optional<TxRequest> port_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* request = std::get_if<RequestTxBurst>(&effects[index]))
      return request->request;
  return std::nullopt;
}

bool has_radio_reset(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (std::holds_alternative<RequestRadioReset>(effects[index])) return true;
  return false;
}

void apply_preferences(test::FakePreferences& preferences,
                       const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* request =
            std::get_if<RequestPersistenceEffect>(&effects[index]))
      preferences.apply(request->request);
}

QC_TEST("ports", "FakeClock supports explicit and wrapped monotonic input") {
  test::FakeClock clock;
  clock.set(10);
  clock.advance(35);
  QC_CHECK_EQ(clock.now_ms(), 45U);
  clock.set(5);
  QC_CHECK_EQ(clock.now_ms(), 5U);

  test::FakeClock wrapping;
  wrapping.ingest_raw32(0xFFFFFFF0U);
  wrapping.ingest_raw32(0x00000010U);
  QC_CHECK_EQ(wrapping.now_ms(), (MonotonicMs{1} << 32U) + 0x10U);
}

QC_TEST("burst", "real transmitter sends three identical spaced packets") {
  test::FakeClock clock;
  test::FakeRadio radio;
  BurstTransmitter transmitter(clock, radio);
  const auto request = port_request();
  QC_CHECK_EQ(transmitter.accept(request), TxAcceptResult::Accepted);
  QC_CHECK_EQ(transmitter.snapshot().phase, BurstPhase::Accepted);

  auto event = transmitter.poll();
  QC_CHECK(event.has_value());
  QC_CHECK(std::holds_alternative<BurstStarted>(*event));
  QC_CHECK_EQ(radio.packets().size(), 0U);
  QC_CHECK_EQ(transmitter.snapshot().phase, BurstPhase::SendingFrame1);

  QC_CHECK(!transmitter.poll().has_value());
  QC_CHECK_EQ(radio.packets().size(), 1U);
  QC_CHECK_EQ(transmitter.snapshot().phase, BurstPhase::Gap1);

  clock.set(kInterFrameGapMs - 1);
  QC_CHECK(!transmitter.poll().has_value());
  QC_CHECK_EQ(radio.packets().size(), 1U);
  clock.set(kInterFrameGapMs);
  QC_CHECK(!transmitter.poll().has_value());
  QC_CHECK_EQ(radio.packets().size(), 2U);
  QC_CHECK_EQ(transmitter.snapshot().phase, BurstPhase::Gap2);

  clock.set(2 * kInterFrameGapMs - 1);
  QC_CHECK(!transmitter.poll().has_value());
  clock.set(2 * kInterFrameGapMs);
  event = transmitter.poll();
  QC_CHECK(event.has_value());
  QC_CHECK(std::holds_alternative<BurstComplete>(*event));
  QC_CHECK_EQ(radio.packets().size(), 3U);
  for (const auto& packet : radio.packets())
    QC_CHECK_EQ(packet, request.payload);
  QC_CHECK_EQ(transmitter.snapshot().phase, BurstPhase::Complete);
  QC_CHECK(!transmitter.poll().has_value());
  QC_CHECK_EQ(transmitter.snapshot().phase, BurstPhase::Idle);
}

QC_TEST("burst", "explicit poll timestamp drives gaps without another clock sample") {
  test::FakeClock clock;
  test::FakeRadio radio;
  BurstTransmitter transmitter(clock, radio);
  QC_CHECK_EQ(transmitter.accept(port_request(10)), TxAcceptResult::Accepted);
  QC_CHECK(std::holds_alternative<BurstStarted>(*transmitter.poll(100)));
  QC_CHECK_EQ(radio.packets().size(), 0U);
  QC_CHECK(!transmitter.poll(100).has_value());
  QC_CHECK_EQ(radio.packets().size(), 1U);
  QC_CHECK(!transmitter.poll(100 + kInterFrameGapMs - 1).has_value());
  QC_CHECK(!transmitter.poll(100 + kInterFrameGapMs).has_value());
  QC_CHECK_EQ(radio.packets().size(), 2U);
}

QC_TEST("burst", "lease revoke is exact and only works before start") {
  test::FakeClock clock;
  test::FakeRadio radio;
  BurstTransmitter transmitter(clock, radio);
  QC_CHECK_EQ(transmitter.accept(port_request(1)), TxAcceptResult::Accepted);
  QC_CHECK_EQ(transmitter.accept(port_request(2)), TxAcceptResult::Busy);
  QC_CHECK(!transmitter.revoke_if_unstarted(TxToken(2)));
  QC_CHECK(transmitter.revoke_if_unstarted(TxToken(1)));
  QC_CHECK_EQ(transmitter.snapshot().phase, BurstPhase::Idle);

  QC_CHECK_EQ(transmitter.accept(port_request(3)), TxAcceptResult::Accepted);
  QC_CHECK(transmitter.poll().has_value());
  QC_CHECK(!transmitter.revoke_if_unstarted(TxToken(3)));
}

QC_TEST("burst", "radio rejection and mid-burst fault never auto-retry") {
  {
    test::FakeClock clock;
    test::FakeRadio radio;
    radio.push_result(RadioSendResult::Rejected);
    BurstTransmitter transmitter(clock, radio);
    transmitter.accept(port_request(4));
    QC_CHECK(std::holds_alternative<BurstStarted>(*transmitter.poll()));
    const auto event = transmitter.poll();
    QC_CHECK(event.has_value());
    QC_CHECK(std::holds_alternative<BurstFault>(*event));
    QC_CHECK_EQ(radio.packets().size(), 1U);
    QC_CHECK_EQ(transmitter.snapshot().phase, BurstPhase::Fault);
  }
  {
    test::FakeClock clock;
    test::FakeRadio radio;
    radio.push_result(RadioSendResult::Sent);
    radio.push_result(RadioSendResult::Fault);
    BurstTransmitter transmitter(clock, radio);
    transmitter.accept(port_request(5));
    QC_CHECK(std::holds_alternative<BurstStarted>(*transmitter.poll()));
    QC_CHECK(!transmitter.poll().has_value());
    clock.set(kInterFrameGapMs);
    const auto event = transmitter.poll();
    QC_CHECK(event.has_value());
    QC_CHECK(std::holds_alternative<BurstFault>(*event));
    QC_CHECK_EQ(radio.packets().size(), 2U);
    clock.advance(10 * kInterFrameGapMs);
    QC_CHECK(!transmitter.poll().has_value());
    QC_CHECK_EQ(radio.packets().size(), 2U);
  }
}

QC_TEST("ports", "recording sink receives typed core events") {
  test::RecordingEventSink sink;
  const CoreEvent event{CoreEventKind::RequestRefused,
                        CoordinatorState::OemHoldoff,
                        RefusalReason::Holdoff, std::nullopt, std::nullopt};
  sink.on_core_event(event);
  QC_CHECK_EQ(sink.events().size(), 1U);
  QC_CHECK_EQ(sink.events()[0].kind, CoreEventKind::RequestRefused);
  QC_CHECK_EQ(sink.events()[0].state, CoordinatorState::OemHoldoff);
}

QC_TEST("ports", "fake burst timelines exercise real core token ownership") {
  ConfirmationCore core(CoreConfig{67});
  RestorableState restored;
  restored.sender = port_sender();
  core.restore(restored, 0);
  core.request_state(FanState::command(Speed::Low, Duration::Off), 0);
  const auto request = port_tx(core.poll(0));
  QC_CHECK(request.has_value());

  test::FakeBurstTransmitter burst;
  QC_CHECK_EQ(burst.accept(*request), TxAcceptResult::Accepted);
  const auto started = burst.started();
  forward(core, started, 0);
  forward(core, burst.duplicate_last(), 1);
  QC_CHECK_EQ(core.snapshot(1).transaction->attempts_started, 1U);
  forward(core, burst.stale_complete(TxToken(999)), 2);
  QC_CHECK_EQ(core.snapshot(2).state, CoordinatorState::CommandTransmitting);
  core.poll(kTxBurstWatchdogMs);
  QC_CHECK_EQ(core.snapshot(kTxBurstWatchdogMs).state,
              CoordinatorState::RadioRecovery);
}

QC_TEST("ports", "mid-burst fault enters started-token radio recovery") {
  ConfirmationCore core(CoreConfig{67});
  RestorableState restored;
  restored.sender = port_sender();
  core.restore(restored, 0);
  core.request_state(FanState::command(Speed::High, Duration::Continuous), 0);
  const auto request = port_tx(core.poll(0));
  QC_CHECK(request.has_value());
  core.on_tx_started(request->token, 1);

  const auto effects = core.on_tx_fault(request->token, 2);
  QC_CHECK_EQ(core.snapshot(2).state, CoordinatorState::RadioRecovery);
  QC_CHECK_EQ(core.snapshot(2).transaction->attempts_started, 1U);
  QC_CHECK(has_radio_reset(effects));

  std::array<CoreEffects, 4> batches;
  for (std::uint8_t attempt = 1; attempt <= 16; ++attempt)
    QC_CHECK(batches[(attempt - 1U) / CoreEffects::kCapacity].add(
        RequestRadioReset{attempt}));
  CoreEffectQueue queue;
  for (std::size_t batch = 0; batch < batches.size(); ++batch)
    QC_CHECK(queue.enqueue(batches[batch], (batch + 1U) * 10U));
  QC_CHECK_EQ(queue.size(), CoreEffectQueue::kCapacity);
  CoreEffects overflow;
  QC_CHECK(overflow.add(RequestRadioReset{17}));
  QC_CHECK(!queue.enqueue(overflow, 50));
  QC_CHECK_EQ(queue.size(), CoreEffectQueue::kCapacity);
  for (std::uint8_t attempt = 1; attempt <= 16; ++attempt) {
    const auto queued = queue.pop();
    QC_CHECK(queued.has_value());
    QC_CHECK_EQ(std::get<RequestRadioReset>(queued->effect).attempt, attempt);
    QC_CHECK_EQ(queued->now_ms, ((attempt - 1U) / 4U + 1U) * 10U);
  }
  QC_CHECK(!queue.pop().has_value());

  CoreEffects initial;
  QC_CHECK(initial.add(RequestRadioReset{1}));
  CoreEffectDrain drain;
  QC_CHECK(drain.enqueue(initial, 100));
  std::vector<std::uint8_t> applied_attempts;
  std::vector<MonotonicMs> publication_times;
  const auto apply = [&](const CoreEffect& effect, MonotonicMs effect_ms) {
    QC_CHECK(drain.draining());
    const auto attempt = std::get<RequestRadioReset>(effect).attempt;
    applied_attempts.push_back(attempt);
    if (attempt == 1) {
      CoreEffects reentrant;
      QC_CHECK(reentrant.add(RequestRadioReset{2}));
      QC_CHECK(drain.enqueue(reentrant, effect_ms + 100));
    }
  };
  const auto publish = [&](MonotonicMs publish_ms) {
    publication_times.push_back(publish_ms);
    if (publication_times.size() == 1) {
      CoreEffects reentrant;
      QC_CHECK(reentrant.add(RequestRadioReset{3}));
      QC_CHECK(drain.enqueue(reentrant, publish_ms + 100));
    } else if (publication_times.size() == 2) {
      CoreEffects empty_reentrant;
      QC_CHECK(drain.enqueue(empty_reentrant, publish_ms + 100));
    }
  };
  drain.drain(apply, publish);
  QC_CHECK_EQ(applied_attempts,
              (std::vector<std::uint8_t>{1, 2, 3}));
  QC_CHECK_EQ(publication_times,
              (std::vector<MonotonicMs>{200, 300, 400}));
  QC_CHECK(!drain.draining());
  QC_CHECK(drain.empty());

  CoreEffects empty;
  QC_CHECK(drain.enqueue(empty, 500));
  drain.drain(apply, publish);
  QC_CHECK_EQ(publication_times,
              (std::vector<MonotonicMs>{200, 300, 400, 500}));
  QC_CHECK(drain.empty());

  CoreEffects timestamp_batch;
  QC_CHECK(timestamp_batch.add(RequestRadioReset{4}));
  QC_CHECK(timestamp_batch.add(RequestRadioReset{5}));
  CoreEffectDrain timestamp_drain;
  QC_CHECK(timestamp_drain.enqueue(timestamp_batch, 600));
  std::vector<MonotonicMs> timestamp_publications;
  const auto apply_with_empty_reentry =
      [&](const CoreEffect& effect, MonotonicMs) {
        if (std::get<RequestRadioReset>(effect).attempt == 4) {
          CoreEffects empty_reentrant;
          QC_CHECK(timestamp_drain.enqueue(empty_reentrant, 700));
        }
      };
  const auto record_timestamp = [&](MonotonicMs publish_ms) {
    timestamp_publications.push_back(publish_ms);
  };
  timestamp_drain.drain(apply_with_empty_reentry, record_timestamp);
  QC_CHECK_EQ(timestamp_publications, (std::vector<MonotonicMs>{700}));

  CoreCallbackQueue callbacks;
  QC_CHECK(callbacks.enqueue(CoreCallbackKind::TxRejected, TxToken(41)));
  QC_CHECK(callbacks.enqueue(CoreCallbackKind::RadioRecovered, std::nullopt));
  const auto rejected_callback = callbacks.pop(900);
  QC_CHECK(rejected_callback.has_value());
  QC_CHECK_EQ(rejected_callback->kind, CoreCallbackKind::TxRejected);
  QC_CHECK_EQ(rejected_callback->token, TxToken(41));
  QC_CHECK_EQ(rejected_callback->now_ms, 900U);
  const auto recovered_callback = callbacks.pop(1000);
  QC_CHECK(recovered_callback.has_value());
  QC_CHECK_EQ(recovered_callback->kind, CoreCallbackKind::RadioRecovered);
  QC_CHECK(!recovered_callback->token.has_value());
  QC_CHECK_EQ(recovered_callback->now_ms, 1000U);
  QC_CHECK(!callbacks.pop(1100).has_value());
  for (std::uint64_t token = 1; token <= CoreCallbackQueue::kCapacity; ++token)
    QC_CHECK(callbacks.enqueue(CoreCallbackKind::TxRejected, TxToken(token)));
  QC_CHECK(!callbacks.enqueue(CoreCallbackKind::TxRejected, TxToken(9)));
  for (std::uint64_t token = 1; token <= CoreCallbackQueue::kCapacity; ++token) {
    const auto callback = callbacks.pop(1100 + token);
    QC_CHECK(callback.has_value());
    QC_CHECK_EQ(callback->token, TxToken(token));
    QC_CHECK_EQ(callback->now_ms, 1100U + token);
  }
  QC_CHECK(!callbacks.pop(1200).has_value());
}

QC_TEST("ports", "core effect batches retain one slot beyond production peak") {
  CoreEffects effects;
  for (std::uint8_t attempt = 1; attempt <= 4; ++attempt)
    QC_CHECK(effects.add(RequestRadioReset{attempt}));
  QC_CHECK(!effects.add(RequestRadioReset{5}));
  QC_CHECK_EQ(effects.size(), 4U);
}

QC_TEST("ports", "fake preferences carries learn and forget across reboot") {
  test::FakePreferences preferences;
  ConfirmationCore learning(CoreConfig{67});
  learning.request_learn(LearnMode::Manual, 0);
  const FrameBytes command{{0xCB, 0x00, 0x47, 0x39, 0x9F, 0x9F}};
  // Issue #6: binding now requires three independent sightings, not two.
  learning.on_frame(ByteView(command.bytes), 1);
  learning.on_frame(ByteView(command.bytes), 602);
  const auto learned = learning.on_frame(ByteView(command.bytes), 1203);
  apply_preferences(preferences, learned);
  preferences.sync();

  ConfirmationCore restored(CoreConfig{67});
  restored.restore(preferences.load(), 0);
  QC_CHECK_EQ(restored.snapshot(0).state, CoordinatorState::Idle);
  const auto forgotten = restored.request_forget(1);
  apply_preferences(preferences, forgotten);
  preferences.sync();

  ConfirmationCore rebooted(CoreConfig{67});
  rebooted.restore(preferences.load(), 0);
  QC_CHECK_EQ(rebooted.snapshot(0).state, CoordinatorState::Unprovisioned);
  QC_CHECK_EQ(preferences.load().seed_policy,
              SeedPolicy::SuppressCompiledSeed);
  QC_CHECK_EQ(preferences.sync_count(), 2U);
}

}  // namespace
}  // namespace quietcool
