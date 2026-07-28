// Every registered publisher must receive every authority snapshot. Before
// this, the component held ONE publisher pointer, so registering a second
// entity silently replaced the first — the fan would have stopped updating
// the moment the timer select was added.

#include "quietcool/esphome/quietcool_component.h"

#include <cmath>
#include <string>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/sensor/sensor.h"

#include "support/test.h"
#include "support/test_doubles.h"

namespace esphome::quietcool {
namespace {

constexpr std::uint32_t kSenderSeed = 0xCB004739U;
constexpr std::uint32_t kPreferenceKey = 0x51434332U;
constexpr std::uint32_t kJitterSeed = 0x51434332U;

class CountingPublisher final : public AuthorityPublisher {
 public:
  void publish_authority(const ::quietcool::AuthoritySnapshot&) override { ++calls; }
  int calls{0};
};

// Drives publication through the real apply_effect(PublishAuthorityEffect)
// path via the existing drive_effects_for_test seam, exactly as the real
// core does when it emits that effect — no test-only publication hook added.
void publish_once(QuietCoolComponent& component) {
  ::quietcool::CoreEffects batch;
  QC_CHECK(batch.add(::quietcool::PublishAuthorityEffect{::quietcool::AuthoritySnapshot{}}));
  component.drive_effects_for_test(batch);
}

QC_TEST("adapter", "every registered publisher receives a snapshot") {
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  CountingPublisher first;
  CountingPublisher second;
  component.add_authority_publisher(&first);
  component.add_authority_publisher(&second);

  publish_once(component);

  // EXACTLY twice (rounds 8-9): an effect-carrying batch delivers in place —
  // so a TransactionFinished later in the same batch sees entities already
  // updated — AND post-drain, which covers the invalidations that emit no
  // effect. Deduplication is every consumer's job (revision, shown option,
  // last-published text); a bare counting publisher sees both deliveries,
  // and pinning the count to 2 is what BINDS the in-place delivery — a
  // weaker >= 1 was satisfied by the post-drain channel alone, so deleting
  // the in-place line reverted the ordering fix with a green suite
  // (round 9, fable + codex).
  QC_CHECK_EQ(first.calls, 2);
  QC_CHECK_EQ(second.calls, 2);
}

QC_TEST("adapter", "an effectless batch still delivers exactly one snapshot") {
  // Pins the POST-DRAIN channel itself (round 9, opus): it is the only
  // channel carrying the invalidations that emit no effect at all — the
  // estimated-timer-deadline path among them — and with only the
  // effect-carrying count asserted, deleting it was provable green before
  // the exact counts landed. One effectless batch, exactly one delivery.
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  CountingPublisher publisher;
  component.add_authority_publisher(&publisher);

  binary_sensor::BinarySensor state_known;
  component.set_state_known_sensor(&state_known);

  ::quietcool::CoreEffects batch;
  QC_CHECK(batch.add(::quietcool::RequestRadioReset{}));
  component.drive_effects_for_test(batch);

  QC_CHECK_EQ(publisher.calls, 1);
  // And the SINK half of the post-drain channel (round 10, opus): it is the
  // only path carrying effect-less invalidations to the six sink-owned
  // entities, and deleting it previously left both suites green.
  QC_CHECK_EQ(state_known.published().size(), std::size_t(1));
}

// Records whether the authority publisher had already been served when the
// sink's Fan State Known publication fires.
class SinkOrderObserver final : public binary_sensor::BinarySensor {
 public:
  const CountingPublisher* publisher{nullptr};
  int publisher_calls_at_publish{-1};
  void publish_state(bool state) override {
    binary_sensor::BinarySensor::publish_state(state);
    if (publisher != nullptr && publisher_calls_at_publish < 0)
      publisher_calls_at_publish = publisher->calls;
  }
};

QC_TEST("adapter", "publishers are served before the sink in one delivery") {
  // Round 10 (codex, high): the sink's *_Known publications fire
  // automations, and one reading the fan entity must see it already at THIS
  // snapshot's state — the sink published first, so a rising Fan State Known
  // automation read a stale fan entity and could transmit from it. The
  // reverse staleness fails closed (a momentarily-false Known flag skips).
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  CountingPublisher publisher;
  SinkOrderObserver state_known;
  state_known.publisher = &publisher;
  component.add_authority_publisher(&publisher);
  component.set_state_known_sensor(&state_known);

  ::quietcool::AuthoritySnapshot confirmed{};
  confirmed.state = ::quietcool::ConfirmedStateAuthority{
      ::quietcool::FanState::command(::quietcool::Speed::High,
                                     ::quietcool::Duration::Continuous),
      ::quietcool::EvidenceSource::PostCommandConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      0,
      2,
      std::nullopt,
      std::nullopt,
      1};
  ::quietcool::CoreEffects batch;
  QC_CHECK(batch.add(::quietcool::PublishAuthorityEffect{confirmed}));
  component.drive_effects_for_test(batch);

  QC_CHECK(state_known.publisher_calls_at_publish >= 1);
}

QC_TEST("adapter", "registering past capacity degrades instead of displacing") {
  // Rounds 3-4: an over-capacity registration is a CONFIG error. Dropping it
  // silently left an entity that transmits but never hears (round 3, codex);
  // mark_failed stopped loop() while leaving the entry points open (round 4,
  // opus). The project's one terminal path is degrade(): latched, loud, and
  // no publisher — existing or new — receives anything afterwards.
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  binary_sensor::BinarySensor fault;
  sensor::Sensor timer_remaining;
  text_sensor::TextSensor evidence_source;
  component.set_controller_fault_sensor(&fault);
  component.set_timer_remaining_sensor(&timer_remaining);
  component.set_evidence_source_sensor(&evidence_source);
  CountingPublisher publishers[QuietCoolComponent::kMaxAuthorityPublishers + 1];
  for (auto& publisher : publishers) component.add_authority_publisher(&publisher);

  publish_once(component);

  // Terminal: the overflow publisher was never swapped in, the Controller
  // Fault problem sensor raised, and the degraded controller publishes to
  // nobody. Timer Remaining and Fan Evidence Source take their terminal
  // values too (round 7, fable + opus: wave 6 added both to the degradation
  // publication and nothing asserted either — deleting them stayed green,
  // leaving HA showing "55 minutes remaining" beside a raised fault).
  QC_CHECK_EQ(publishers[QuietCoolComponent::kMaxAuthorityPublishers].calls, 0);
  QC_CHECK(!fault.published().empty());
  QC_CHECK(fault.published().back());
  QC_CHECK(!timer_remaining.published().empty());
  QC_CHECK(std::isnan(timer_remaining.published().back()));
  QC_CHECK(!evidence_source.published().empty());
  QC_CHECK_EQ(evidence_source.published().back(), std::string("unavailable"));
  QC_CHECK_EQ(publishers[0].calls, 0);
}

QC_TEST("adapter", "a fault sensor registered after degradation still raises") {
  // Generated setup wires fan: before binary_sensor:, so a degrade() fired
  // during publisher registration (the overflow path) predates the fault
  // sensor's existence. The setter must replay the latched state or the
  // "Controller Fault raises" promise silently depends on registration order
  // (round 5, all three finding engines).
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  CountingPublisher publishers[QuietCoolComponent::kMaxAuthorityPublishers + 1];
  for (auto& publisher : publishers) component.add_authority_publisher(&publisher);

  // Degraded before the sensor exists; registration must replay it.
  binary_sensor::BinarySensor fault;
  component.set_controller_fault_sensor(&fault);

  QC_CHECK(!fault.published().empty());
  QC_CHECK(fault.published().back());
}

QC_TEST("adapter", "timer remaining registered alone after degradation replays NaN") {
  // ONLY this sensor registers (round 8, opus): with several sensors
  // registered in sequence, a later setter's replay publishes to ALL of them,
  // so an earlier sensor's assertion passed even with its own replay line
  // deleted — the registration order was doing the work, not the code under
  // test. One sensor per case leaves nothing to mask it.
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  CountingPublisher publishers[QuietCoolComponent::kMaxAuthorityPublishers + 1];
  for (auto& publisher : publishers) component.add_authority_publisher(&publisher);

  sensor::Sensor timer_remaining;
  component.set_timer_remaining_sensor(&timer_remaining);

  QC_CHECK(!timer_remaining.published().empty());
  QC_CHECK(std::isnan(timer_remaining.published().back()));
}

QC_TEST("adapter", "evidence source registered alone after degradation replays unavailable") {
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  CountingPublisher publishers[QuietCoolComponent::kMaxAuthorityPublishers + 1];
  for (auto& publisher : publishers) component.add_authority_publisher(&publisher);

  text_sensor::TextSensor evidence_source;
  component.set_evidence_source_sensor(&evidence_source);

  QC_CHECK(!evidence_source.published().empty());
  QC_CHECK_EQ(evidence_source.published().back(), std::string("unavailable"));
}

// Observes what Fan State Known reads at the exact moment "confirmed"
// publishes — the mid-batch ordering wave 9's sink delivery exists for.
class StatusOrderObserver final : public text_sensor::TextSensor {
 public:
  binary_sensor::BinarySensor* state_known{nullptr};
  bool state_known_true_at_confirmed{false};
  void publish_state(const std::string& value) override {
    text_sensor::TextSensor::publish_state(value);
    if (value == "confirmed" && state_known != nullptr &&
        !state_known->published().empty())
      state_known_true_at_confirmed = state_known->published().back();
  }
};

QC_TEST("adapter", "state known is already true when confirmed publishes in the same batch") {
  // Round 10 (fable): the sink half of the in-place delivery was deletable —
  // the exact-count tests count only AuthorityPublisher calls, and the sink
  // is not a publisher. This observer runs INSIDE the batch, at the moment
  // Command Confirmation Status publishes "confirmed", and requires Fan
  // State Known to have already been published true by the in-place sink
  // delivery. With only the post-drain channel, it reads the PREVIOUS
  // batch's false.
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  binary_sensor::BinarySensor state_known;
  StatusOrderObserver status;
  status.state_known = &state_known;
  component.set_state_known_sensor(&state_known);
  component.set_command_status_sensor(&status);

  ::quietcool::AuthoritySnapshot confirmed{};
  confirmed.state = ::quietcool::ConfirmedStateAuthority{
      ::quietcool::FanState::command(::quietcool::Speed::High,
                                     ::quietcool::Duration::Continuous),
      ::quietcool::EvidenceSource::PostCommandConsensus,
      ::quietcool::EvidenceConfidence::ExactBackedConsensus,
      0,
      2,
      std::nullopt,
      std::nullopt,
      1};
  ::quietcool::CoreEffects batch;
  QC_CHECK(batch.add(::quietcool::PublishAuthorityEffect{confirmed}));
  QC_CHECK(batch.add(::quietcool::PublishCoreEvent{
      {::quietcool::CoreEventKind::TransactionFinished,
       ::quietcool::CoordinatorState::Idle,
       std::nullopt,
       ::quietcool::TransactionOutcome::Confirmed,
       std::nullopt}}));
  component.drive_effects_for_test(batch);

  QC_CHECK(!status.published().empty());
  QC_CHECK_EQ(status.published().back(), std::string("confirmed"));
  QC_CHECK(status.state_known_true_at_confirmed);
}

}  // namespace
}  // namespace esphome::quietcool
