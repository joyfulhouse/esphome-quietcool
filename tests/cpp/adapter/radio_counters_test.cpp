// Adapter-layer tests for the restored TX/RX radio counters (Task 3).
//
// See docs/claude/2026-07-28-timer-control-and-diagnostics-design.md section
// 3.5: `TX Count` is the instrument that once proved this bridge was not
// jamming the OEM remote — a flat TX Count through the remote's retry storm
// showed the storm was not this bridge's own traffic. Losing it removed the
// evidence. This file drives the real ConfirmationCore, BurstTransmitter and
// event sink through the component's public entry points; only ESPHome's
// entity, logging, preferences and timing surface is stubbed.
//
// RX_VALID_COUNT / RX_REJECTED_COUNT: see the task report
// (.superpowers/sdd/2026-07-28-timer-control-and-diagnostics-plan/task-3-report.md)
// for why only the TX counter is exercised here. In short: `on_radio_packet`
// has no accept/reject branch of its own — it is a single unconditional call
// into `ConfirmationCore::on_frame`, and the accept/reject decision
// (`ResponseClassifier::classify` landing on `InvalidOrIrrelevant` or not) is
// made entirely inside core, which this task may not modify. The candidate
// proxy — treating an empty `CoreEffects` return as "rejected" — is not
// faithful: `confirmation_reducer.cpp`'s TrackCandidate handling returns `{}`
// for a genuinely accepted `LocalResponseCandidate` every time consensus has
// not yet been reached, which `consensus_tracker.cpp` requires 2-3 independent
// candidates for — meaning the first accepted response frame in literally
// every exchange would be miscounted as rejected. That is not an edge case;
// it is the common case, and coding it up would ship an inaccurate instrument
// of exactly the kind this diagnostic exists to prevent.

#include "quietcool/esphome/quietcool_component.h"

#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"

#include "support/test.h"
#include "support/test_doubles.h"

#include <cstddef>
#include <cstdint>

namespace esphome::quietcool {
namespace {

constexpr std::uint32_t kSenderSeed = 0xCB004739U;
constexpr std::uint32_t kPreferenceKey = 0x51434332U;
constexpr std::uint32_t kJitterSeed = 0x51434332U;

// Owns the stub NVS for one test and restores the global on the way out, so
// tests cannot leak provisioning state into each other. Copied from
// component_deferral_test.cpp: needed only because setup() (below) reaches
// EspHomePreferencesAdapter::load(), which touches global_preferences.
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

QC_TEST("adapter", "tx_count_increments_once_per_completed_burst") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey,
                               kJitterSeed);
  host_test::set_millis(0);

  component.setup();
  QC_CHECK_EQ(component.tx_count(), std::uint32_t(0));

  // on_radio_ready() (inside setup()) issues the boot query — the first real
  // burst the core ever transmits. It is 3 physical frames (kInterFrameGapMs
  // apart), which BurstTransmitter completes as a single BurstComplete event.
  // Drive real loop() passes, exactly as component_deferral_test.cpp's boot
  // test does, until all 3 frames are on the wire.
  for (std::size_t pass = 0; pass < 30 && radio.packets().size() < 3;
       ++pass) {
    host_test::advance_millis(50);
    component.call_loop();
  }

  QC_CHECK_EQ(radio.packets().size(), std::size_t(3));
  // One completed burst, not one increment per physical frame.
  QC_CHECK_EQ(component.tx_count(), std::uint32_t(1));

  // Further passes with no incoming RF and the response window still open
  // must not transmit again — tx_count_ must stay flat, exactly the property
  // that matters for the OEM-jamming diagnostic.
  for (std::size_t pass = 0; pass < 5; ++pass) {
    host_test::advance_millis(10);
    component.call_loop();
  }
  QC_CHECK_EQ(component.tx_count(), std::uint32_t(1));
}

}  // namespace
}  // namespace esphome::quietcool
