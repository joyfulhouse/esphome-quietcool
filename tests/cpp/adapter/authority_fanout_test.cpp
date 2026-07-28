// Every registered publisher must receive every authority snapshot. Before
// this, the component held ONE publisher pointer, so registering a second
// entity silently replaced the first — the fan would have stopped updating
// the moment the timer select was added.

#include "quietcool/esphome/quietcool_component.h"

#include "esphome/components/binary_sensor/binary_sensor.h"

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

  QC_CHECK_EQ(first.calls, 1);
  QC_CHECK_EQ(second.calls, 1);
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
  component.set_controller_fault_sensor(&fault);
  CountingPublisher publishers[QuietCoolComponent::kMaxAuthorityPublishers + 1];
  for (auto& publisher : publishers) component.add_authority_publisher(&publisher);

  publish_once(component);

  // Terminal: the overflow publisher was never swapped in, the Controller
  // Fault problem sensor raised, and the degraded controller publishes to
  // nobody.
  QC_CHECK_EQ(publishers[QuietCoolComponent::kMaxAuthorityPublishers].calls, 0);
  QC_CHECK(!fault.published().empty());
  QC_CHECK(fault.published().back());
  QC_CHECK_EQ(publishers[0].calls, 0);
}

}  // namespace
}  // namespace esphome::quietcool
