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
// RX_VALID_COUNT / RX_REJECTED_COUNT are FRAME-VALIDATION counters, not
// classifier-relevance counters — see the task-3 report for the full history.
// `on_radio_packet` has no accept/reject branch of its own to observe (it is
// a single unconditional call into `ConfirmationCore::on_frame`), and the
// classifier's accept/reject verdict is not recoverable from on_frame's
// returned CoreEffects without misclassifying the common case (a
// still-accumulating consensus candidate legitimately returns empty
// effects). The legacy YAML build settled what these counters actually
// measured: whether the frame decodes against the provisioned sender
// (`FrameCodec::decode_strict`, a pure static function — calling it here is
// not a core modification, and duplicates only a cheap decode of a ~10-byte
// frame rather than reaching into core's private classification state).

#include "quietcool/esphome/quietcool_component.h"

#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"

#include "support/test.h"
#include "support/test_doubles.h"

#include <array>
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

QC_TEST("adapter", "rx_valid_count_increments_on_an_accepted_frame") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey,
                               kJitterSeed);
  host_test::set_millis(0);
  // Binds provisioned_sender_ via the compiled seed (kSenderSeed), since NVS
  // is empty — see preferences_adapter.cpp's apply_compiled_seed.
  component.setup();

  QC_CHECK_EQ(component.rx_valid_count(), std::uint32_t(0));
  QC_CHECK_EQ(component.rx_rejected_count(), std::uint32_t(0));

  // The exact 6-byte OEM query for kSenderSeed (0xCB004739), matching
  // frame_codec_test.cpp's "query uses exact six-byte wire order" — a frame
  // FrameCodec::decode_strict genuinely accepts against this sender.
  const std::array<std::uint8_t, 6> query_frame{0xCB, 0x00, 0x47, 0x39,
                                                0x66, 0x66};
  component.on_radio_packet(
      ::quietcool::ByteView(query_frame.data(), query_frame.size()));

  QC_CHECK_EQ(component.rx_valid_count(), std::uint32_t(1));
  QC_CHECK_EQ(component.rx_rejected_count(), std::uint32_t(0));
}

QC_TEST("adapter", "rx_rejected_count_increments_on_a_rejected_frame") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey,
                               kJitterSeed);
  host_test::set_millis(0);
  component.setup();

  QC_CHECK_EQ(component.rx_valid_count(), std::uint32_t(0));
  QC_CHECK_EQ(component.rx_rejected_count(), std::uint32_t(0));

  // A 5-byte frame: FrameCodec::decode_strict genuinely rejects every length
  // other than 6 (FrameDecodeError::InvalidLength) — see
  // frame_codec_test.cpp's "strict decoder rejects lengths sender tails and
  // states". Not a guess at what looks malformed; pinned by that test.
  const std::array<std::uint8_t, 5> short_frame{0xCB, 0x00, 0x47, 0x39, 0x66};
  component.on_radio_packet(
      ::quietcool::ByteView(short_frame.data(), short_frame.size()));

  QC_CHECK_EQ(component.rx_rejected_count(), std::uint32_t(1));
  QC_CHECK_EQ(component.rx_valid_count(), std::uint32_t(0));
}

QC_TEST("adapter",
        "rx_rejected_count_increments_while_unprovisioned_with_no_sender_to_validate") {
  // No ScopedPreferences/setup(): provisioned_sender_ stays nullopt, exactly
  // like an unprovisioned unit (bare construction, matching
  // authority_fanout_test.cpp's pattern for a test that never calls setup()).
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey,
                               kJitterSeed);

  const std::array<std::uint8_t, 6> query_frame{0xCB, 0x00, 0x47, 0x39,
                                                0x66, 0x66};
  component.on_radio_packet(
      ::quietcool::ByteView(query_frame.data(), query_frame.size()));

  // Nothing to validate against, so counted rejected rather than dropped
  // silently from the diagnostic — matches legacy's sender-mismatch outcome.
  QC_CHECK_EQ(component.rx_rejected_count(), std::uint32_t(1));
  QC_CHECK_EQ(component.rx_valid_count(), std::uint32_t(0));
}

}  // namespace
}  // namespace esphome::quietcool
