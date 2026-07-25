// Adapter-layer tests for QuietCoolComponent.
//
// This file is the reason the ESPHome stubs exist. Until now the adapter half
// of the loop-stack crash fix had no host coverage at all: the core half is
// pinned by a recursion probe and the drain/queue primitives are unit-tested,
// but the wiring that actually defers core re-entry — in quietcool_component.cpp
// — was verified only by `esphome compile` and by the firmware not crashing.
//
// The property under test is the one whose absence corrupted FreeRTOS kernel
// structures on live hardware: applying an effect must never re-enter the core
// on the same stack. RequestTxBurst hitting a busy transmitter, and
// RequestRadioReset recovering the radio, both used to call straight back into
// the core from inside apply_effect(). They now enqueue onto a bounded FIFO
// that loop() drains at top level, which converts recursion into iteration
// across loop passes.
//
// Everything below drives the real ConfirmationCore, the real BurstTransmitter,
// the real drain and the real callback queue. Only ESPHome's entity, logging,
// preferences and timing surface is stubbed.

#include "quietcool/esphome/quietcool_component.h"

#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"

#include "support/test.h"
#include "support/test_doubles.h"

#include <cstdint>

namespace esphome::quietcool {
namespace {

using ::quietcool::CoordinatorState;
using ::quietcool::Duration;
using ::quietcool::FanState;
using ::quietcool::RadioRecoveryResult;
using ::quietcool::RadioSendResult;
using ::quietcool::Speed;

constexpr std::uint32_t kSenderSeed = 0xCB004739U;
constexpr std::uint32_t kPreferenceKey = 0x51434332U;
constexpr std::uint32_t kJitterSeed = 0x51434332U;

// Owns the stub NVS for one test and restores the global on the way out, so
// tests cannot leak provisioning state into each other.
class ScopedPreferences final {
 public:
  ScopedPreferences() {
    previous_ = global_preferences;
    global_preferences = &preferences_;
  }
  ~ScopedPreferences() { global_preferences = previous_; }

  ESPPreferences& get() { return preferences_; }

 private:
  ESPPreferences preferences_;
  ESPPreferences* previous_{nullptr};
};

// Drives the component the way ESPHome would: a millisecond clock the test
// advances explicitly, and loop() calls that respect the failed-component rule.
class Harness final {
 public:
  Harness() : component_(&radio_, kSenderSeed, kPreferenceKey, kJitterSeed) {
    host_test::set_millis(0);
  }

  void setup() { component_.setup(); }

  void advance_and_loop(std::uint32_t delta_ms) {
    host_test::advance_millis(delta_ms);
    component_.call_loop();
  }

  // Runs loop() until the coordinator reaches `target`, or gives up. Returns
  // the number of iterations, so a test can assert that work was deferred
  // across passes rather than completed inline.
  std::size_t loop_until(CoordinatorState target, std::size_t limit,
                         std::uint32_t step_ms = 10) {
    for (std::size_t iteration = 1; iteration <= limit; ++iteration) {
      advance_and_loop(step_ms);
      if (state() == target) return iteration;
    }
    return 0;
  }

  CoordinatorState state() const { return component_.snapshot().state; }

  ::quietcool::test::FakeRadio& radio() { return radio_; }
  QuietCoolComponent& component() { return component_; }

 private:
  ::quietcool::test::FakeRadio radio_;
  QuietCoolComponent component_;
};

// A transmit fault is the cleanest way to reach RequestRadioReset through the
// real core: the boot query bursts, the radio faults, the core begins radio
// recovery and asks the adapter to reset the radio.
void drive_to_radio_recovery(Harness& harness) {
  harness.radio().push_result(RadioSendResult::Fault);
  harness.setup();
  QC_CHECK(harness.loop_until(CoordinatorState::RadioRecovery, 20) != 0);
}

QC_TEST("adapter", "radio recovery is deferred to a later loop, not re-entered") {
  ScopedPreferences preferences;
  Harness harness;

  // Recovery must succeed the moment it is attempted, so that if the adapter
  // re-entered the core inline the state change would land inside that very
  // same loop pass — which is precisely what this test rules out.
  harness.radio().set_recovery_result(RadioRecoveryResult::Recovered);
  harness.radio().push_result(RadioSendResult::Fault);
  harness.setup();

  // Step one pass at a time and catch the pass in which recover() is called.
  std::size_t recovery_pass = 0;
  CoordinatorState state_after_recovery_pass = CoordinatorState::Idle;
  std::size_t before = harness.radio().recovery_count();
  for (std::size_t pass = 1; pass <= 60; ++pass) {
    harness.advance_and_loop(10);
    if (harness.radio().recovery_count() > before) {
      recovery_pass = pass;
      state_after_recovery_pass = harness.state();
      break;
    }
    before = harness.radio().recovery_count();
  }

  QC_CHECK(recovery_pass != 0);

  // The pass that reset the radio must NOT have advanced the coordinator past
  // RadioRecovery: on_radio_recovered() is queued, not called on that stack.
  // Before the fix this call chain was apply_effect -> radio_.recover() ->
  // core_.on_radio_recovered() -> more effects -> apply_effects again, nesting
  // the core inside its own effect application.
  QC_CHECK_EQ(state_after_recovery_pass, CoordinatorState::RadioRecovery);
}

QC_TEST("adapter", "deferred callback advances the core on a later pass") {
  ScopedPreferences preferences;
  Harness harness;
  drive_to_radio_recovery(harness);
  harness.radio().set_recovery_result(RadioRecoveryResult::Recovered);

  // Given enough passes the deferred RadioRecovered callback is drained and the
  // coordinator leaves RadioRecovery. This is the other half of the contract:
  // deferral must not mean the work is dropped.
  bool left_recovery = false;
  for (std::size_t pass = 0; pass < 200; ++pass) {
    harness.advance_and_loop(25);
    if (harness.state() != CoordinatorState::RadioRecovery) {
      left_recovery = true;
      break;
    }
  }
  QC_CHECK(left_recovery);
  QC_CHECK(!harness.component().is_failed());
}

QC_TEST("adapter", "a healthy boot never marks the component failed") {
  ScopedPreferences preferences;
  Harness harness;
  harness.setup();
  for (std::size_t pass = 0; pass < 100; ++pass) harness.advance_and_loop(25);

  // No overflow, no fail-closed path taken on an ordinary boot-and-idle.
  QC_CHECK(!harness.component().is_failed());
}

QC_TEST("adapter", "mark_failed is terminal: loop never runs again") {
  ScopedPreferences preferences;
  Harness harness;
  harness.setup();
  const auto state_before = harness.state();

  harness.component().mark_failed();
  QC_CHECK(harness.component().is_failed());
  QC_CHECK(harness.component().status_has_error());

  // ESPHome removes a failed component from the loop and never restores it, so
  // the coordinator must be frozen from here. This is why the production YAML
  // carries a Restart button: nothing recovers this at runtime.
  for (std::size_t pass = 0; pass < 50; ++pass) harness.advance_and_loop(100);
  QC_CHECK_EQ(harness.state(), state_before);
  QC_CHECK(harness.component().is_failed());
}

QC_TEST("adapter", "radio recovery fault does not mark the component failed") {
  ScopedPreferences preferences;
  Harness harness;
  drive_to_radio_recovery(harness);
  harness.radio().set_recovery_result(RadioRecoveryResult::Fault);

  // A radio that refuses to recover is a runtime condition the core handles by
  // budget, not an adapter invariant violation. Only queue overflow may fail
  // the component; a persistent radio fault must not.
  for (std::size_t pass = 0; pass < 100; ++pass) harness.advance_and_loop(25);
  QC_CHECK(!harness.component().is_failed());
}

QC_TEST("adapter", "survives an empty NVS without failing or bricking setup") {
  ScopedPreferences preferences;
  Harness harness;
  harness.setup();

  // Nothing was ever stored: the adapter must come up unprovisioned rather than
  // restore a wrong sender, and must not fail the component.
  QC_CHECK(!harness.component().is_failed());
  for (std::size_t pass = 0; pass < 20; ++pass) harness.advance_and_loop(50);
  QC_CHECK(!harness.component().is_failed());
}

QC_TEST("adapter", "survives a corrupt NVS record") {
  ScopedPreferences preferences;
  {
    Harness first;
    first.setup();
    for (std::size_t pass = 0; pass < 20; ++pass) first.advance_and_loop(50);
  }
  preferences.get().corrupt(kPreferenceKey);

  Harness harness;
  harness.setup();
  QC_CHECK(!harness.component().is_failed());
  for (std::size_t pass = 0; pass < 20; ++pass) harness.advance_and_loop(50);
  QC_CHECK(!harness.component().is_failed());
}

QC_TEST("adapter", "boot transmits only the query, never a state command") {
  ScopedPreferences preferences;
  Harness harness;
  harness.setup();
  for (std::size_t pass = 0; pass < 60; ++pass) harness.advance_and_loop(25);

  // The 0xAF-at-MEDIUM incident during the first cutover was never explained by
  // any code path. This pins the property that boot RF is query-only: every
  // byte the adapter transmits before any user request must be the 0x66 query,
  // never an outbound state command (which always carries the 0x80 marker).
  // Bytes 0-3 are the sender ID; bytes 4-5 carry the repeated command byte.
  QC_CHECK(!harness.radio().packets().empty());
  for (const auto& packet : harness.radio().packets())
    for (std::size_t index = 4; index < packet.bytes.size(); ++index)
      QC_CHECK_EQ(packet.bytes[index], std::uint8_t(0x66));
}

// M2 (issue #9): a fault sensor whose publish_state re-enters the component,
// used to prove the degraded_ latch is set before any degradation publication.
class ReentrantFaultSensor final : public binary_sensor::BinarySensor {
 public:
  void set_target(QuietCoolComponent* component) { component_ = component; }
  void publish_state(bool state) override {
    binary_sensor::BinarySensor::publish_state(state);
    if (component_ != nullptr)
      component_->request_state(
          FanState::command(Speed::High, Duration::Continuous));
  }

 private:
  QuietCoolComponent* component_{nullptr};
};

// Wires the full degradation entity surface onto a component.
struct DegradationSensors final {
  binary_sensor::BinarySensor fault;
  binary_sensor::BinarySensor state_known;
  binary_sensor::BinarySensor timer_program_known;
  binary_sensor::BinarySensor timer_remaining_known;
  binary_sensor::BinarySensor confirmed_off;
  text_sensor::TextSensor command_status;

  void wire(QuietCoolComponent& component) {
    component.set_controller_fault_sensor(&fault);
    component.set_state_known_sensor(&state_known);
    component.set_timer_program_known_sensor(&timer_program_known);
    component.set_timer_remaining_known_sensor(&timer_remaining_known);
    component.set_confirmed_off_sensor(&confirmed_off);
    component.set_command_status_sensor(&command_status);
  }

  // Every "known" claim drops to false, the problem sensor raises, and
  // command_status reads "unavailable": stale confirmed authority is
  // invalidated so Home Assistant stops presenting it as trustworthy.
  void check_degraded() const {
    QC_CHECK(!fault.published().empty());
    QC_CHECK(fault.published().back());
    QC_CHECK(!state_known.published().empty());
    QC_CHECK(!state_known.published().back());
    QC_CHECK(!timer_program_known.published().empty());
    QC_CHECK(!timer_program_known.published().back());
    QC_CHECK(!timer_remaining_known.published().empty());
    QC_CHECK(!timer_remaining_known.published().back());
    QC_CHECK(!confirmed_off.published().empty());
    QC_CHECK(!confirmed_off.published().back());
    QC_CHECK(!command_status.published().empty());
    QC_CHECK_EQ(command_status.published().back(), std::string("unavailable"));
  }
};

QC_TEST("adapter", "effect queue overflow degrades visibly and terminally") {
  ScopedPreferences preferences;
  Harness harness;
  harness.setup();

  DegradationSensors sensors;
  sensors.wire(harness.component());

  harness.component().force_effect_queue_overflow_for_test();

  QC_CHECK(harness.component().is_failed());
  sensors.check_degraded();
}

QC_TEST("adapter", "callback queue overflow degrades visibly and terminally") {
  ScopedPreferences preferences;
  Harness harness;
  harness.setup();

  DegradationSensors sensors;
  sensors.wire(harness.component());

  harness.component().force_callback_queue_overflow_for_test();

  QC_CHECK(harness.component().is_failed());
  sensors.check_degraded();
}

QC_TEST("adapter", "degrade latches before publishing and refuses reentrant requests") {
  ScopedPreferences preferences;
  Harness harness;
  harness.setup();

  ReentrantFaultSensor fault;
  text_sensor::TextSensor command_status;
  fault.set_target(&harness.component());
  harness.component().set_controller_fault_sensor(&fault);
  harness.component().set_command_status_sensor(&command_status);

  harness.component().force_callback_queue_overflow_for_test();

  // degraded_ was latched before publish_controller_failed(), so the fault
  // sensor's re-entrant request_state() hit the guard and never drove the core:
  // no "refused"/"pending" status was produced, only the terminal
  // "unavailable". degrade() is idempotent, so the fault published exactly once.
  QC_CHECK(harness.component().is_failed());
  QC_CHECK_EQ(fault.published().size(), std::size_t(1));
  QC_CHECK(fault.published().back());
  for (const auto& status : command_status.published())
    QC_CHECK(status != "refused" && status != "pending");
  QC_CHECK(!command_status.published().empty());
  QC_CHECK_EQ(command_status.published().back(), std::string("unavailable"));
}

}  // namespace
}  // namespace esphome::quietcool
