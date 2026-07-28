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

  // AT LEAST once, not exactly once (round 8, codex): an effect-carrying
  // batch delivers in place — so a TransactionFinished later in the same
  // batch sees entities already updated — AND post-drain, which covers the
  // invalidations that emit no effect. Deduplication is every consumer's
  // job, and each one has a gate (revision, shown option, last-published
  // text); a bare counting publisher like this one sees both deliveries.
  QC_CHECK(first.calls >= 1);
  QC_CHECK_EQ(first.calls, second.calls);
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

}  // namespace
}  // namespace esphome::quietcool
