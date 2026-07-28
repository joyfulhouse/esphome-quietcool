// Adapter-layer tests for the five restored text diagnostics and the
// publication wiring for the three restored counters (Task 4). See
// docs/claude/2026-07-28-timer-control-and-diagnostics-design.md section 3.5.
//
// Entities publish only CONFIRMED authority: `Last Confirmed Fan State` reads
// "unknown" rather than the last diagnostic guess whenever authority is not a
// ConfirmedStateAuthority, and `Fan Speed Capability` reads "unknown" rather
// than a fabricated band when the sticky capability is empty — the exact
// defect class behind issue #30/#31, where a wrong speed band stopped a real
// fan. The pure formatting (diagnostic_format.h) is tested directly here as
// well as through the real component, following the discipline
// timer_command_test.cpp established for Task 1's mapping.

#include "quietcool/esphome/quietcool_component.h"

#include "quietcool/esphome/diagnostic_format.h"

#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"

#include "support/test.h"
#include "support/test_doubles.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace esphome::quietcool {
namespace {

using ::quietcool::AuthoritySnapshot;
using ::quietcool::ConfirmedStateAuthority;
using ::quietcool::Duration;
using ::quietcool::EvidenceConfidence;
using ::quietcool::EvidenceSource;
using ::quietcool::FanState;
using ::quietcool::SenderId;
using ::quietcool::Speed;
using ::quietcool::SpeedCapability;
using ::quietcool::TimerLossReason;
using ::quietcool::UnknownStateAuthority;
using ::quietcool::UnknownTimerAuthority;

constexpr std::uint32_t kSenderSeed = 0xCB004739U;
constexpr std::uint32_t kPreferenceKey = 0x51434332U;
constexpr std::uint32_t kJitterSeed = 0x51434332U;

// Owns the stub NVS for one test and restores the global on the way out —
// copied from radio_counters_test.cpp / component_deferral_test.cpp.
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

AuthoritySnapshot confirmed_snapshot(FanState state,
                                     std::optional<SpeedCapability> capability) {
  AuthoritySnapshot snapshot{};
  snapshot.state = ConfirmedStateAuthority{
      state, EvidenceSource::ManualQueryConsensus,
      EvidenceConfidence::ExactBackedConsensus, 0, 3,
      std::nullopt, std::nullopt, 1};
  snapshot.timer = UnknownTimerAuthority{TimerLossReason::Unknown, 0};
  snapshot.speed_capability = capability;
  snapshot.revision = 1;
  return snapshot;
}

// Drives publication through publish_authority_for_test, which calls the SAME
// publish_authority_snapshot the production post-drain lambda calls. The
// former vehicle — a hand-built PublishAuthorityEffect through
// drive_effects_for_test — stopped reaching entities when round 2 moved
// production delivery to the post-drain core snapshot (whose source is the
// real core, un-injectable from here).
void publish(QuietCoolComponent& component, const AuthoritySnapshot& authority) {
  component.publish_authority_for_test(authority);
}

// ---------------------------------------------------------------------------
// Pure formatting (diagnostic_format.h), tested directly.
// ---------------------------------------------------------------------------

QC_TEST("adapter", "format_hex_byte renders a two-digit uppercase byte") {
  QC_CHECK_EQ(format_hex_byte(0xB1), std::string("0xB1"));
  QC_CHECK_EQ(format_hex_byte(0x00), std::string("0x00"));
  QC_CHECK_EQ(format_hex_byte(0x66), std::string("0x66"));
}

QC_TEST("adapter", "format_sender_id renders the provisioned sender or unknown") {
  QC_CHECK_EQ(format_sender_id(std::nullopt), std::string("unknown"));
  const auto sender = SenderId::from_be_u32(kSenderSeed).value();
  QC_CHECK_EQ(format_sender_id(sender), std::string("0xCB004739"));
}

QC_TEST("adapter", "format_speed_capability never fabricates a band when empty") {
  QC_CHECK_EQ(format_speed_capability(std::nullopt), std::string("unknown"));
  QC_CHECK_EQ(format_speed_capability(SpeedCapability::Unknown),
              std::string("unknown"));
  QC_CHECK_EQ(format_speed_capability(SpeedCapability::One), std::string("1"));
  QC_CHECK_EQ(format_speed_capability(SpeedCapability::Two), std::string("2"));
  QC_CHECK_EQ(format_speed_capability(SpeedCapability::Three), std::string("3"));
}

QC_TEST("adapter", "format_last_confirmed_state formats a running speed with its duration") {
  QC_CHECK_EQ(format_last_confirmed_state(ConfirmedStateAuthority{
                  FanState::command(Speed::High, Duration::Hours1),
                  EvidenceSource::ManualQueryConsensus,
                  EvidenceConfidence::ExactBackedConsensus, 0, 3, std::nullopt,
                  std::nullopt, 1}),
              std::string("high 1h"));
}

QC_TEST("adapter", "format_last_confirmed_state omits the suffix while running continuously") {
  QC_CHECK_EQ(format_last_confirmed_state(ConfirmedStateAuthority{
                  FanState::command(Speed::Medium, Duration::Continuous),
                  EvidenceSource::ManualQueryConsensus,
                  EvidenceConfidence::ExactBackedConsensus, 0, 3, std::nullopt,
                  std::nullopt, 1}),
              std::string("medium"));
}

QC_TEST("adapter", "format_last_confirmed_state reports off with no duration suffix") {
  QC_CHECK_EQ(format_last_confirmed_state(ConfirmedStateAuthority{
                  FanState::command(Speed::Low, Duration::Off),
                  EvidenceSource::ManualQueryConsensus,
                  EvidenceConfidence::ExactBackedConsensus, 0, 3, std::nullopt,
                  std::nullopt, 1}),
              std::string("off"));
}

QC_TEST("adapter", "format_last_confirmed_state publishes unknown when authority is not confirmed") {
  QC_CHECK_EQ(format_last_confirmed_state(
                  UnknownStateAuthority{::quietcool::AuthorityLossReason::Boot, 0,
                                       std::nullopt, std::nullopt}),
              std::string("unknown"));
}

// ---------------------------------------------------------------------------
// Publication through the real component.
// ---------------------------------------------------------------------------

QC_TEST("adapter", "last confirmed fan state sensor publishes speed and duration") {
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  text_sensor::TextSensor sensor;
  component.set_last_confirmed_state_sensor(&sensor);

  publish(component, confirmed_snapshot(FanState::command(Speed::High, Duration::Hours1),
                                        SpeedCapability::Three));

  QC_CHECK(!sensor.published().empty());
  QC_CHECK_EQ(sensor.published().back(), std::string("high 1h"));
}

QC_TEST("adapter", "last confirmed fan state sensor publishes unknown while unconfirmed") {
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  text_sensor::TextSensor sensor;
  component.set_last_confirmed_state_sensor(&sensor);

  // Default AuthoritySnapshot{} holds UnknownStateAuthority (its variant's
  // first alternative), exactly as authority_fanout_test.cpp relies on.
  publish(component, AuthoritySnapshot{});

  QC_CHECK(!sensor.published().empty());
  QC_CHECK_EQ(sensor.published().back(), std::string("unknown"));
}

QC_TEST("adapter", "fan speed capability sensor publishes the confirmed band") {
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  text_sensor::TextSensor sensor;
  component.set_speed_capability_sensor(&sensor);

  publish(component, confirmed_snapshot(FanState::command(Speed::High, Duration::Continuous),
                                        SpeedCapability::Two));

  QC_CHECK(!sensor.published().empty());
  QC_CHECK_EQ(sensor.published().back(), std::string("2"));
}

// The named mutation target from the task brief: an empty capability must
// never publish a fabricated "3".
QC_TEST("adapter", "fan speed capability sensor publishes unknown never a fabricated three when empty") {
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  text_sensor::TextSensor sensor;
  component.set_speed_capability_sensor(&sensor);

  publish(component, confirmed_snapshot(FanState::command(Speed::High, Duration::Continuous),
                                        std::nullopt));

  QC_CHECK(!sensor.published().empty());
  QC_CHECK_EQ(sensor.published().back(), std::string("unknown"));
  QC_CHECK(sensor.published().back() != "3");
}

QC_TEST("adapter", "last tx command sensor publishes the outbound state byte as uppercase hex") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  text_sensor::TextSensor sensor;
  component.set_last_tx_command_sensor(&sensor);
  host_test::set_millis(0);
  component.setup();

  // Overrides the pending boot query outright (matches
  // component_deferral_test.cpp's "setup after degrade" pattern): HIGH + 1
  // hour == 0xB1 per fan_state.cpp's table.
  component.request_state(FanState::command(Speed::High, Duration::Hours1));

  for (std::size_t pass = 0; pass < 30 && radio.packets().size() < 3; ++pass) {
    host_test::advance_millis(50);
    component.call_loop();
  }

  QC_CHECK_EQ(radio.packets().size(), std::size_t(3));
  QC_CHECK(!sensor.published().empty());
  QC_CHECK_EQ(sensor.published().back(), std::string("0xB1"));
}

// Guards the TxReason::TransactionCommand filter: a query burst (0x66) must
// never be mistaken for a command byte.
QC_TEST("adapter", "last tx command sensor is untouched by the boot query") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  text_sensor::TextSensor sensor;
  component.set_last_tx_command_sensor(&sensor);
  host_test::set_millis(0);
  component.setup();

  for (std::size_t pass = 0; pass < 30 && radio.packets().size() < 3; ++pass) {
    host_test::advance_millis(50);
    component.call_loop();
  }

  QC_CHECK(!radio.packets().empty());
  QC_CHECK(sensor.published().empty());
}

QC_TEST("adapter", "last valid rx frame sensor publishes state frames as uppercase hex") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  text_sensor::TextSensor sensor;
  component.set_last_rx_frame_sensor(&sensor);
  host_test::set_millis(0);
  component.setup();

  // A state frame (0x9F = LOW|Continuous, duplicated trailer) — the kind of
  // decode this diagnostic exists for.
  const std::array<std::uint8_t, 6> state_frame{0xCB, 0x00, 0x47, 0x39, 0x9F, 0x9F};
  component.on_radio_packet(
      ::quietcool::ByteView(state_frame.data(), state_frame.size()));

  QC_CHECK(!sensor.published().empty());
  QC_CHECK_EQ(sensor.published().back(), std::string("0x9F"));
}

QC_TEST("adapter", "last valid rx frame sensor ignores query frames") {
  // An ExactQuery's marker byte is 0x66 — a protocol marker, not a fan state.
  // Publishing it made "Last Valid RX Frame" display 0x66 every time the OEM
  // remote polled (adversarial round 1, opus). The frame still counts as
  // VALID; only the text sensor skips it.
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  text_sensor::TextSensor sensor;
  component.set_last_rx_frame_sensor(&sensor);
  host_test::set_millis(0);
  component.setup();

  const std::array<std::uint8_t, 6> query_frame{0xCB, 0x00, 0x47, 0x39, 0x66, 0x66};
  component.on_radio_packet(
      ::quietcool::ByteView(query_frame.data(), query_frame.size()));

  QC_CHECK(sensor.published().empty());
  QC_CHECK_EQ(component.rx_valid_count(), std::uint32_t(1));
}

QC_TEST("adapter", "last valid rx frame sensor is untouched by a rejected frame") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  text_sensor::TextSensor sensor;
  component.set_last_rx_frame_sensor(&sensor);
  host_test::set_millis(0);
  component.setup();

  const std::array<std::uint8_t, 5> short_frame{0xCB, 0x00, 0x47, 0x39, 0x66};
  component.on_radio_packet(
      ::quietcool::ByteView(short_frame.data(), short_frame.size()));

  QC_CHECK(sensor.published().empty());
}

QC_TEST("adapter", "remote sender id sensor publishes the provisioned sender in hex") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  text_sensor::TextSensor sensor;
  component.set_remote_sender_id_sensor(&sensor);
  host_test::set_millis(0);

  // Binds provisioned_sender_ via the compiled seed, since NVS is empty.
  component.setup();

  QC_CHECK(!sensor.published().empty());
  QC_CHECK_EQ(sensor.published().back(), std::string("0xCB004739"));
}

QC_TEST("adapter", "remote sender id sensor publishes unknown once the binding is forgotten") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  text_sensor::TextSensor sensor;
  component.set_remote_sender_id_sensor(&sensor);
  host_test::set_millis(0);
  component.setup();

  component.request_forget();

  QC_CHECK(!sensor.published().empty());
  QC_CHECK_EQ(sensor.published().back(), std::string("unknown"));
}

// ---------------------------------------------------------------------------
// The three restored counters (Task 3 added tx_count()/rx_valid_count()/
// rx_rejected_count(); this task wires the numeric sensors).
// ---------------------------------------------------------------------------

QC_TEST("adapter", "tx count sensor publishes the counter value on burst completion") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  sensor::Sensor tx_count_sensor;
  component.set_tx_count_sensor(&tx_count_sensor);
  host_test::set_millis(0);
  component.setup();

  for (std::size_t pass = 0; pass < 30 && radio.packets().size() < 3; ++pass) {
    host_test::advance_millis(50);
    component.call_loop();
  }

  QC_CHECK_EQ(component.tx_count(), std::uint32_t(1));
  QC_CHECK(!tx_count_sensor.published().empty());
  QC_CHECK_EQ(tx_count_sensor.published().back(), 1.0F);
}

QC_TEST("adapter", "rx valid count sensor publishes the counter value on an accepted frame") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  sensor::Sensor rx_valid_sensor;
  component.set_rx_valid_count_sensor(&rx_valid_sensor);
  host_test::set_millis(0);
  component.setup();

  const std::array<std::uint8_t, 6> query_frame{0xCB, 0x00, 0x47, 0x39, 0x66, 0x66};
  component.on_radio_packet(
      ::quietcool::ByteView(query_frame.data(), query_frame.size()));

  QC_CHECK_EQ(component.rx_valid_count(), std::uint32_t(1));
  // Counter entities publish from loop()'s paced flush, not per packet
  // (round 2): the increment is immediate, the entity update arrives on the
  // next loop tick past the pacing window.
  host_test::advance_millis(1001);
  component.call_loop();
  QC_CHECK(!rx_valid_sensor.published().empty());
  QC_CHECK_EQ(rx_valid_sensor.published().back(), 1.0F);
}

QC_TEST("adapter", "rx rejected count sensor publishes the counter value on a rejected frame") {
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  sensor::Sensor rx_rejected_sensor;
  component.set_rx_rejected_count_sensor(&rx_rejected_sensor);
  host_test::set_millis(0);
  component.setup();

  const std::array<std::uint8_t, 5> short_frame{0xCB, 0x00, 0x47, 0x39, 0x66};
  component.on_radio_packet(
      ::quietcool::ByteView(short_frame.data(), short_frame.size()));

  QC_CHECK_EQ(component.rx_rejected_count(), std::uint32_t(1));
  host_test::advance_millis(1001);
  component.call_loop();
  QC_CHECK(!rx_rejected_sensor.published().empty());
  QC_CHECK_EQ(rx_rejected_sensor.published().back(), 1.0F);
}

QC_TEST("adapter", "a storm's final rejected total is flushed after the storm ends") {
  // The round-2 defect all three engines converged on: per-packet throttling
  // dropped intermediate publishes AND the final one, so a 200-frame noise
  // burst could leave Home Assistant reading 1 until the next rejection —
  // days later. The loop flush publishes the settled total.
  ScopedPreferences preferences;
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, kSenderSeed, kPreferenceKey, kJitterSeed);
  sensor::Sensor rx_rejected_sensor;
  component.set_rx_rejected_count_sensor(&rx_rejected_sensor);
  host_test::set_millis(0);
  component.setup();

  const std::array<std::uint8_t, 5> short_frame{0xCB, 0x00, 0x47, 0x39, 0x66};
  for (int i = 0; i < 200; ++i) {
    component.on_radio_packet(
        ::quietcool::ByteView(short_frame.data(), short_frame.size()));
    host_test::advance_millis(20);  // 200 frames in ~4 s, inside one window
    component.call_loop();
  }
  // Storm over; quiet RF. The flush must still deliver the final total.
  host_test::advance_millis(1001);
  component.call_loop();
  QC_CHECK_EQ(component.rx_rejected_count(), std::uint32_t(200));
  QC_CHECK(!rx_rejected_sensor.published().empty());
  QC_CHECK_EQ(rx_rejected_sensor.published().back(), 200.0F);
}

}  // namespace
}  // namespace esphome::quietcool
