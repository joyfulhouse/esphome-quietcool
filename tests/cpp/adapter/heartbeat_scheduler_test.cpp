#include "quietcool/esphome/quietcool_component.h"

#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"
#include "support/test.h"
#include "support/test_doubles.h"

#include <cstdint>
#include <limits>

namespace esphome::quietcool {
namespace {

constexpr std::uint32_t kSenderSeed = 0xCB004739U;
constexpr std::uint32_t kPreferenceKey = 0x51434332U;
constexpr std::uint32_t kJitterSeed = 0x13579BDFU;

class ScopedPreferences final {
 public:
  ScopedPreferences() {
    previous_ = global_preferences;
    global_preferences = &preferences_;
  }
  ~ScopedPreferences() { global_preferences = previous_; }

 private:
  ESPPreferences preferences_;
  ESPPreferences* previous_{nullptr};
};

class HeartbeatHarness final {
 public:
  explicit HeartbeatHarness(std::uint32_t interval_ms = 300000)
      : component_(&radio_, kSenderSeed, kPreferenceKey, kJitterSeed,
                   interval_ms) {}

  void setup(std::uint32_t raw_millis = 0) {
    host_test::set_millis(raw_millis);
    component_.setup();
  }

  void loop_after(std::uint32_t delta_ms) {
    host_test::advance_millis(delta_ms);
    component_.call_loop();
  }

  bool reach_idle(std::size_t limit = 300) {
    for (std::size_t pass = 0; pass < limit; ++pass) {
      if (component_.snapshot().state == ::quietcool::CoordinatorState::Idle)
        return true;
      loop_after(25);
    }
    return component_.snapshot().state == ::quietcool::CoordinatorState::Idle;
  }

  QuietCoolComponent& component() { return component_; }

 private:
  ::quietcool::test::FakeRadio radio_;
  QuietCoolComponent component_;
};

void publish_recent_passive_authority(QuietCoolComponent& component) {
  constexpr std::uint8_t report[]{0xCB, 0x00, 0x47, 0x39, 0x1F, 0x1F};
  component.on_radio_packet(::quietcool::ByteView(report, sizeof(report)));
  host_test::advance_millis(::quietcool::kMinIndependentCandidateGapMs);
  component.on_radio_packet(::quietcool::ByteView(report, sizeof(report)));
}

QC_TEST("heartbeat_scheduler",
        "default waits five minutes and zero disables scheduling") {
  ScopedPreferences preferences;
  HeartbeatHarness enabled;
  enabled.setup();
  QC_CHECK(enabled.reach_idle());
  const auto now = enabled.component().now_ms();
  enabled.loop_after(static_cast<std::uint32_t>(299999U - now));
  QC_CHECK_EQ(enabled.component().snapshot().heartbeat_queries_admitted, 0U);
  enabled.loop_after(1);
  QC_CHECK_EQ(enabled.component().snapshot().heartbeat_queries_admitted, 1U);

  HeartbeatHarness disabled(0);
  disabled.setup();
  QC_CHECK(disabled.reach_idle());
  disabled.loop_after(3600000);
  QC_CHECK_EQ(disabled.component().snapshot().heartbeat_queries_admitted, 0U);
  QC_CHECK(!disabled.component().next_heartbeat_due_for_test().has_value());
}

QC_TEST("heartbeat_scheduler",
        "busy due time skips once and does not queue catch-up") {
  ScopedPreferences preferences;
  HeartbeatHarness harness(60000);
  harness.setup();
  QC_CHECK(harness.reach_idle());
  const auto now = harness.component().now_ms();
  harness.loop_after(static_cast<std::uint32_t>(59999U - now));
  harness.component().request_state(::quietcool::FanState::command(
      ::quietcool::Speed::Low, ::quietcool::Duration::Continuous));
  harness.loop_after(1);
  auto snapshot = harness.component().snapshot();
  QC_CHECK_EQ(snapshot.heartbeat_queries_skipped_busy, 1U);
  const auto next = harness.component().next_heartbeat_due_for_test();
  QC_CHECK(next.has_value());
  QC_CHECK(*next > harness.component().now_ms());
  for (std::size_t pass = 0; pass < 10; ++pass) harness.loop_after(0);
  snapshot = harness.component().snapshot();
  QC_CHECK_EQ(snapshot.heartbeat_queries_skipped_busy, 1U);
}

QC_TEST("heartbeat_scheduler",
        "recent passive authority suppresses the due heartbeat") {
  ScopedPreferences preferences;
  HeartbeatHarness harness(60000);
  harness.setup();
  QC_CHECK(harness.reach_idle());
  const auto now = harness.component().now_ms();
  harness.loop_after(static_cast<std::uint32_t>(59000U - now));
  publish_recent_passive_authority(harness.component());
  harness.loop_after(940);
  const auto snapshot = harness.component().snapshot();
  QC_CHECK_EQ(snapshot.heartbeat_queries_suppressed_recent, 1U);
  QC_CHECK_EQ(snapshot.heartbeat_queries_admitted, 0U);
}

QC_TEST("heartbeat_scheduler",
        "deterministic jitter is bounded and honors the sixty-second floor") {
  bool below_base = false;
  bool above_base = false;
  for (std::uint32_t sequence = 0; sequence < 128; ++sequence) {
    const auto delay = QuietCoolComponent::heartbeat_delay_for_test(
        300000, kJitterSeed, sequence, 1000000);
    QC_CHECK(delay >= 270000);
    QC_CHECK(delay <= 330000);
    QC_CHECK_EQ(delay, QuietCoolComponent::heartbeat_delay_for_test(
                           300000, kJitterSeed, sequence,
                           1000000));
    below_base = below_base || delay < 300000;
    above_base = above_base || delay > 300000;

    const auto minimum = QuietCoolComponent::heartbeat_delay_for_test(
        60000, kJitterSeed, sequence, 1000000);
    QC_CHECK(minimum >= 60000);
    QC_CHECK(minimum <= 66000);
  }
  QC_CHECK(below_base);
  QC_CHECK(above_base);
}

QC_TEST("heartbeat_scheduler",
        "first due time remains correct across raw millis wrap") {
  ScopedPreferences preferences;
  HeartbeatHarness harness(60000);
  harness.setup(std::numeric_limits<std::uint32_t>::max() - 1000U);
  QC_CHECK(harness.reach_idle());
  const auto due = harness.component().next_heartbeat_due_for_test();
  QC_CHECK(due.has_value());
  const auto now = harness.component().now_ms();
  QC_CHECK(*due > now);
  harness.loop_after(static_cast<std::uint32_t>(*due - now - 1U));
  QC_CHECK_EQ(harness.component().snapshot().heartbeat_queries_admitted, 0U);
  harness.loop_after(1);
  QC_CHECK_EQ(harness.component().snapshot().heartbeat_queries_admitted, 1U);
}

}  // namespace
}  // namespace esphome::quietcool
