// Adapter-layer tests for EspHomePreferencesAdapter (issue #31).
//
// The stored record gained a speed_capability byte by repurposing the reserved
// byte under a NEW flag bit while keeping kAdapterSchemaVersion at 1. That
// cross-version compatibility is the point: a version bump would make a
// ROLLBACK read the record as not intact and boot fail-closed into
// SuppressCompiledSeed — an unprovisioned fan after a downgrade. These tests
// pin the byte-level layout, the flag gating, the fail-closed path for a
// corrupt capability value, and that Forget erases the capability (it may
// precede re-learning a DIFFERENT fan).
//
// They also pin the two halves of the capability's DURABILITY, which are in
// tension: the value must reach flash immediately (a staged-only save is lost
// to exactly the ungraceful power cut #31 exists to survive), and it must
// reach flash only when the stored record actually changes (every confirmed
// report re-confirms the capability, so an unconditional commit would put a
// flash erase/write cycle behind each one). The stub NVS models the RAM stage
// and simulate_power_loss() so a test can tell those apart.
//
// Several tests drive the REAL component through the real adapter and core:
// a persisted capability must reach the authority publisher during setup(),
// before any RF round-trip — the #9 lesson ("verify the config binds the
// fix") applied to #31's window.

#include "quietcool/esphome/quietcool_component.h"

#include "quietcool/esphome/fan_command.h"
#include "quietcool/esphome/fan_feedback.h"
#include "quietcool/esphome/preferences_adapter.h"

#include "esphome/core/hal.h"
#include "esphome/core/preferences.h"

#include "support/test.h"
#include "support/test_doubles.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <variant>

namespace esphome::quietcool {
namespace {

using ::quietcool::PersistenceKind;
using ::quietcool::PersistenceRequest;
using ::quietcool::SeedPolicy;
using ::quietcool::SenderId;
using ::quietcool::SpeedCapability;

constexpr std::uint32_t kPreferenceKey = 0x51434333U;
// A distinct test-only sender; only the 0xCB prefix is meaningful.
constexpr std::uint32_t kSenderBe = 0xCB0011EEU;

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

SenderId sender() { return SenderId::from_be_u32(kSenderBe).value(); }

bool provision(EspHomePreferencesAdapter& adapter) {
  return adapter.apply(PersistenceRequest{PersistenceKind::SaveProvisioning,
                                          sender(), std::nullopt,
                                          std::nullopt});
}

bool save_capability(EspHomePreferencesAdapter& adapter,
                     SpeedCapability capability) {
  return adapter.apply(PersistenceRequest{PersistenceKind::SaveSpeedCapability,
                                          std::nullopt, std::nullopt,
                                          capability});
}

bool save_remembered_speed(EspHomePreferencesAdapter& adapter,
                           ::quietcool::Speed speed) {
  return adapter.apply(PersistenceRequest{
      PersistenceKind::SaveRememberedSpeed, std::nullopt, speed, std::nullopt});
}

// Byte-level mirror of the persisted record. Field-for-field identical to the
// adapter's private StoredRecord (whose layout is pinned by static_asserts in
// preferences_adapter.h); used to hand-craft records the way OLD firmware
// wrote them, and corrupt ones no encoder would produce.
struct RawRecord final {
  std::uint32_t magic;
  std::uint16_t adapter_schema_version;
  std::uint16_t core_schema_version;
  std::uint8_t flags;
  std::uint8_t seed_policy;
  std::uint8_t remembered_speed;
  std::uint8_t speed_capability;
  std::uint32_t sender_be;
  std::uint32_t checksum;
};
static_assert(sizeof(RawRecord) == 20, "mirror must match StoredRecord");

std::uint32_t fnv_checksum(const RawRecord& record) {
  std::uint32_t value = 2166136261U;
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&record);
  for (std::size_t index = 0;
       index < sizeof(RawRecord) - sizeof(record.checksum); ++index) {
    value ^= bytes[index];
    value *= 16777619U;
  }
  return value;
}

void store_raw(RawRecord record) {
  record.checksum = fnv_checksum(record);
  auto preference =
      global_preferences->make_preference<RawRecord>(kPreferenceKey, true);
  QC_CHECK(preference.save(&record));
}

QC_TEST("preferences", "capability round-trips through the stored record") {
  ScopedPreferences preferences;
  {
    EspHomePreferencesAdapter adapter(kPreferenceKey, 0);
    adapter.load();
    QC_CHECK(provision(adapter));
    QC_CHECK(save_capability(adapter, SpeedCapability::Two));
  }
  // A fresh adapter (a rebooted device) sees the capability again.
  EspHomePreferencesAdapter reborn(kPreferenceKey, 0);
  const auto restored = reborn.load();
  QC_CHECK(restored.sender.has_value());
  QC_CHECK_EQ(restored.sender->as_be_u32(), kSenderBe);
  QC_CHECK_EQ(restored.speed_capability.value(), SpeedCapability::Two);
  QC_CHECK_EQ(restored.seed_policy, SeedPolicy::AllowCompiledSeed);
}

// The durability half. ESPHome's ESP32 backend only STAGES a save in RAM;
// nothing reaches flash until sync(), which the preferences component's
// interval syncer calls on a default 60 s flash_write_interval. A capability
// learned seconds after boot and merely staged is therefore lost to exactly
// the ungraceful power cut issue #31 exists to survive, which reopens the
// unknown-capability window on a fan whose band was already proven.
//
// Reverting SaveSpeedCapability to a non-durable write fails this test at the
// has_value() line below.
QC_TEST("preferences", "a learned capability survives a power cut inside the flash interval") {
  ScopedPreferences preferences;
  {
    EspHomePreferencesAdapter adapter(kPreferenceKey, 0);
    adapter.load();
    QC_CHECK(provision(adapter));
    QC_CHECK(save_capability(adapter, SpeedCapability::Two));
    // Staged AFTER the last commit, and deliberately non-durable: losing a
    // remembered speed costs one OEM-faithful byte on the next command, not a
    // reopened band window. It is the contrast that proves this harness can
    // tell a committed value from a staged one.
    QC_CHECK(save_remembered_speed(adapter, ::quietcool::Speed::High));
  }
  // The power cut lands before the interval syncer's next pass.
  preferences.get().simulate_power_loss();

  EspHomePreferencesAdapter reborn(kPreferenceKey, 0);
  const auto restored = reborn.load();
  QC_CHECK(restored.speed_capability.has_value());
  QC_CHECK_EQ(restored.speed_capability.value(), SpeedCapability::Two);
  QC_CHECK(restored.sender.has_value());
  QC_CHECK(!restored.remembered_speed.has_value());
}

// The other half, and the reason durability is not simply "sync on every
// durable request": the fan re-confirms its capability on EVERY report, and a
// flash erase/write cycle per report would wear NVS out. Two independent
// filters stop it — the core emits SaveSpeedCapability only on a change
// (ConfirmationCore::promote_authority), and the adapter commits only when the
// ENCODED RECORD changes. This pins the adapter's filter directly, by handing
// it the re-assertions the core's filter would normally absorb.
QC_TEST("preferences", "a re-asserted capability costs no flash write") {
  ScopedPreferences preferences;
  EspHomePreferencesAdapter adapter(kPreferenceKey, 0);
  adapter.load();
  QC_CHECK(provision(adapter));
  QC_CHECK(save_capability(adapter, SpeedCapability::Two));

  const auto syncs = preferences.get().sync_count();
  const auto writes = preferences.get().write_count(kPreferenceKey);
  for (int report = 0; report < 64; ++report)
    QC_CHECK(save_capability(adapter, SpeedCapability::Two));
  // Not one staged byte and not one commit, for 64 confirmed reports.
  QC_CHECK_EQ(preferences.get().write_count(kPreferenceKey), writes);
  QC_CHECK_EQ(preferences.get().sync_count(), syncs);

  // A capability that actually changes still reaches flash at once.
  QC_CHECK(save_capability(adapter, SpeedCapability::Three));
  QC_CHECK_EQ(preferences.get().write_count(kPreferenceKey), writes + 1);
  QC_CHECK_EQ(preferences.get().sync_count(), syncs + 1);
  preferences.get().simulate_power_loss();
  EspHomePreferencesAdapter reborn(kPreferenceKey, 0);
  QC_CHECK_EQ(reborn.load().speed_capability.value(), SpeedCapability::Three);
}

// Suppressing the commit when the record is unchanged must not swallow a
// commit that FAILED. After a failed sync the record is staged and stored_
// already matches it, so an unchanged-record shortcut would report success
// over a capability still living only in RAM — durable in name only. The
// adapter tracks the outstanding commit instead of inferring it from
// `changed`, so the next durable request flushes it.
QC_TEST("preferences", "a failed flash commit is retried by the next durable request") {
  ScopedPreferences preferences;
  EspHomePreferencesAdapter adapter(kPreferenceKey, 0);
  adapter.load();
  QC_CHECK(provision(adapter));

  preferences.get().set_sync_result(false);
  QC_CHECK(!save_capability(adapter, SpeedCapability::Two));  // staged only
  preferences.get().set_sync_result(true);
  // The same capability again: nothing about the record changed, but its
  // commit is still owed.
  QC_CHECK(save_capability(adapter, SpeedCapability::Two));

  preferences.get().simulate_power_loss();
  EspHomePreferencesAdapter reborn(kPreferenceKey, 0);
  const auto restored = reborn.load();
  QC_CHECK(restored.speed_capability.has_value());
  QC_CHECK_EQ(restored.speed_capability.value(), SpeedCapability::Two);
}

QC_TEST("preferences", "old record without the capability flag loads cleanly") {
  ScopedPreferences preferences;
  // Exactly what version-1 firmware wrote: flags without kHasSpeedCapability
  // (1<<2) and an arbitrary reserved byte — its value was never defined, so it
  // must be ignored, not decoded.
  RawRecord record{};
  record.magic = 0x51435032U;
  record.adapter_schema_version = 1;
  record.core_schema_version = 1;
  record.flags = 0x03U;  // kHasSender | kHasRememberedSpeed
  record.seed_policy = 0;
  record.remembered_speed = 3;
  record.speed_capability = 0xA5U;  // garbage in the old reserved byte
  record.sender_be = kSenderBe;
  store_raw(record);

  EspHomePreferencesAdapter adapter(kPreferenceKey, 0);
  const auto restored = adapter.load();
  QC_CHECK(restored.sender.has_value());
  QC_CHECK_EQ(restored.remembered_speed.value(), ::quietcool::Speed::High);
  QC_CHECK(!restored.speed_capability.has_value());
  QC_CHECK_EQ(restored.seed_policy, SeedPolicy::AllowCompiledSeed);
}

QC_TEST("preferences", "corrupt capability value fails the whole record closed") {
  ScopedPreferences preferences;
  RawRecord record{};
  record.magic = 0x51435032U;
  record.adapter_schema_version = 1;
  record.core_schema_version = 1;
  record.flags = 0x05U;  // kHasSender | kHasSpeedCapability
  record.speed_capability = 7;  // outside {1, 2, 3}, checksum still valid
  record.sender_be = kSenderBe;
  store_raw(record);

  EspHomePreferencesAdapter adapter(kPreferenceKey, 0);
  const auto restored = adapter.load();
  // safe_suppressed_state: no sender, no capability, compiled seed suppressed.
  QC_CHECK(!restored.sender.has_value());
  QC_CHECK(!restored.speed_capability.has_value());
  QC_CHECK_EQ(restored.seed_policy, SeedPolicy::SuppressCompiledSeed);
}

QC_TEST("preferences", "forget erases the persisted capability") {
  ScopedPreferences preferences;
  {
    EspHomePreferencesAdapter adapter(kPreferenceKey, 0);
    adapter.load();
    QC_CHECK(provision(adapter));
    QC_CHECK(save_capability(adapter, SpeedCapability::Two));
    QC_CHECK(adapter.apply(PersistenceRequest{
        PersistenceKind::EraseProvisioning, std::nullopt, std::nullopt,
        std::nullopt}));
  }
  EspHomePreferencesAdapter reborn(kPreferenceKey, 0);
  const auto restored = reborn.load();
  QC_CHECK(!restored.sender.has_value());
  QC_CHECK(!restored.speed_capability.has_value());
}

// Issue #31 (review finding): SaveProvisioning re-binds the whole record, so
// its capability field is authoritative. Learning a DIFFERENT fan emits it
// with no capability, and the adapter must NOT re-persist the previous fan's
// value under the new sender — that stale value would survive reboots.
QC_TEST("preferences", "re-provisioning replaces the persisted capability") {
  ScopedPreferences preferences;
  constexpr std::uint32_t kOtherSenderBe = 0xCB0011EFU;
  {
    EspHomePreferencesAdapter adapter(kPreferenceKey, 0);
    adapter.load();
    QC_CHECK(provision(adapter));
    QC_CHECK(save_capability(adapter, SpeedCapability::Two));
    // Re-learned to a different fan: the core's sticky value was cleared, so
    // the request carries nullopt.
    QC_CHECK(adapter.apply(PersistenceRequest{
        PersistenceKind::SaveProvisioning,
        SenderId::from_be_u32(kOtherSenderBe).value(), std::nullopt,
        std::nullopt}));
  }
  EspHomePreferencesAdapter reborn(kPreferenceKey, 0);
  const auto restored = reborn.load();
  QC_CHECK_EQ(restored.sender->as_be_u32(), kOtherSenderBe);
  QC_CHECK(!restored.speed_capability.has_value());

  // And a same-fan re-learn (core kept its sticky value) carries it through.
  {
    EspHomePreferencesAdapter adapter(kPreferenceKey, 0);
    adapter.load();
    QC_CHECK(adapter.apply(PersistenceRequest{
        PersistenceKind::SaveProvisioning,
        SenderId::from_be_u32(kOtherSenderBe).value(), std::nullopt,
        SpeedCapability::Three}));
  }
  EspHomePreferencesAdapter again(kPreferenceKey, 0);
  QC_CHECK_EQ(again.load().speed_capability.value(), SpeedCapability::Three);
}

QC_TEST("preferences", "SaveSpeedCapability without a value is refused") {
  ScopedPreferences preferences;
  EspHomePreferencesAdapter adapter(kPreferenceKey, 0);
  adapter.load();
  QC_CHECK(provision(adapter));
  QC_CHECK(!adapter.apply(PersistenceRequest{
      PersistenceKind::SaveSpeedCapability, std::nullopt, std::nullopt,
      std::nullopt}));
}

class CapturingPublisher final : public AuthorityPublisher {
 public:
  void publish_authority(
      const ::quietcool::AuthoritySnapshot& authority) override {
    last = authority;
    ++publications;
  }

  std::optional<::quietcool::AuthoritySnapshot> last;
  std::size_t publications{0};
};

// Drives the component's loop until it reaches `target`, in 25 ms steps.
bool loop_to(QuietCoolComponent& component, ::quietcool::CoordinatorState target,
             std::size_t limit) {
  for (std::size_t pass = 0; pass < limit; ++pass) {
    if (component.snapshot().state == target) return true;
    host_test::advance_millis(25);
    component.call_loop();
  }
  return component.snapshot().state == target;
}

// Feeds the same 6-byte report twice, far enough apart to count as independent
// candidates, then lets the effects drain.
void confirm_with(QuietCoolComponent& component,
                  const std::array<std::uint8_t, 6>& report) {
  host_test::advance_millis(500);
  component.on_radio_packet(::quietcool::ByteView(report.data(), 6));
  host_test::advance_millis(100);
  component.on_radio_packet(::quietcool::ByteView(report.data(), 6));
  component.call_loop();
}

// The whole #31 chain on the host: NVS record -> adapter load -> core restore
// -> PublishAuthorityEffect -> authority publisher, all inside setup(), before
// a single frame is transmitted. Then the published capability must make a
// level-2 command transmit HIGH, not MED.
QC_TEST("preferences", "restored capability reaches the publisher before any RF") {
  ScopedPreferences preferences;
  {
    EspHomePreferencesAdapter writer(kPreferenceKey, 0);
    writer.load();
    QC_CHECK(provision(writer));
    QC_CHECK(save_capability(writer, SpeedCapability::Two));
  }

  host_test::set_millis(0);
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, 0, kPreferenceKey, 59);
  CapturingPublisher publisher;
  component.add_authority_publisher(&publisher);
  component.setup();

  QC_CHECK(publisher.last.has_value());
  QC_CHECK(std::holds_alternative<::quietcool::UnknownStateAuthority>(
      publisher.last->state));
  QC_CHECK_EQ(publisher.last->speed_capability.value(), SpeedCapability::Two);

  // A level-2 press mapped against the seeded bands transmits HIGH (0xBF), and
  // the entity is listed as a 2-level fan from the very first ListEntities.
  FanSpeedBands bands;
  bands.observe(publisher.last->speed_capability);
  QC_CHECK_EQ(bands.entity(), 2);
  QC_CHECK_EQ(bands.command(), 2);
  QC_CHECK_EQ(
      fan_command_from_intent(true, 2, bands.command()).outbound_command_byte(),
      0xBF);
}

// The other half of the #31 chain, on a unit that has never learned a band:
// a 2-speed fan's confirmation must CONFIRM the entity band AND reach NVS, so
// the unknown-capability window is entered at most once per fan. The fan's
// confirming report is byte-identical to the command frame (both carry marker
// bits 10), which is exactly the frame the echo filter cannot attribute — if
// that evidence is discarded rather than ranked, nothing is ever persisted and
// every boot re-enters the window on a fan whose band was already provable.
QC_TEST("preferences", "a two-speed confirmation narrows the entity and reaches NVS") {
  ScopedPreferences preferences;
  {
    EspHomePreferencesAdapter writer(kPreferenceKey, 0);
    writer.load();
    QC_CHECK(provision(writer));  // provisioned, capability unknown
    QC_CHECK(!writer.load().speed_capability.has_value());
  }

  host_test::set_millis(0);
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, 0, kPreferenceKey, 59);
  CapturingPublisher publisher;
  component.add_authority_publisher(&publisher);
  component.setup();
  // The entity is listed at the WIDEST band while nothing is known, so learning
  // can only narrow it; the command band is the MED-free one.
  FanSpeedBands bands;
  bands.observe(publisher.last->speed_capability);
  QC_CHECK_EQ(bands.entity(), kUnlearnedEntitySpeedCount);
  QC_CHECK_EQ(bands.command(), kUnlearnedCommandSpeedCount);
  const int listed_at_connect = bands.entity();

  // Let the unanswered boot query lapse, then command LOW.
  QC_CHECK(loop_to(component, ::quietcool::CoordinatorState::Idle, 2000));
  component.request_state(
      ::quietcool::FanState::command(::quietcool::Speed::Low,
                                     ::quietcool::Duration::Continuous));
  QC_CHECK(loop_to(component,
                   ::quietcool::CoordinatorState::PostCommandListening, 2000));
  const auto& sent = radio.packets().back();
  QC_CHECK_EQ(sent.bytes[4], 0x9F);

  // Two independent confirmations inside the acceptance window, byte-identical
  // to what we transmitted — a genuine 2-speed report.
  confirm_with(component, {sent.bytes[0], sent.bytes[1], sent.bytes[2],
                           sent.bytes[3], 0x9F, 0x9F});

  QC_CHECK(publisher.last->speed_capability.has_value());
  QC_CHECK_EQ(publisher.last->speed_capability.value(), SpeedCapability::Two);
  bands.observe(publisher.last->speed_capability);
  QC_CHECK_EQ(bands.entity(), 2);  // narrowed, which Home Assistant survives
  QC_CHECK_EQ(bands.command(), 2);
  // Home Assistant is still working in the band it was listed at connect time;
  // its top-of-band press must transmit HIGH, not MED.
  QC_CHECK_EQ(fan_command_from_intent(true, listed_at_connect, bands.command())
                  .outbound_command_byte(),
              0xBF);
  QC_CHECK_EQ(
      fan_command_from_intent(true, 2, bands.command()).outbound_command_byte(),
      0xBF);

  // And it is durable, so the next boot never reopens the window — including
  // the boot that follows a power cut inside the 60 s flash_write_interval,
  // which is the cut this whole persistence path exists to survive.
  preferences.get().simulate_power_loss();
  EspHomePreferencesAdapter reader(kPreferenceKey, 0);
  const auto reloaded = reader.load().speed_capability;
  QC_CHECK(reloaded.has_value());
  QC_CHECK_EQ(reloaded.value(), SpeedCapability::Two);
}

// The no-flash-write-per-report property through the REAL core and component,
// which is where the claim has to hold: the fan re-confirms its capability on
// every exchange. This drives four full command/confirm rounds against a fan
// whose capability is already stored and requires that not one byte is staged
// and not one commit is issued. Together with the adapter-level test above it
// covers both filters — the core's (promote_authority emits only on a change)
// and the adapter's (commit only when the encoded record changes).
QC_TEST("preferences", "repeated confirmations of a known capability cost no flash write") {
  ScopedPreferences preferences;
  {
    EspHomePreferencesAdapter writer(kPreferenceKey, 0);
    writer.load();
    QC_CHECK(provision(writer));
    QC_CHECK(save_capability(writer, SpeedCapability::Two));
  }

  host_test::set_millis(0);
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, 0, kPreferenceKey, 59);
  CapturingPublisher publisher;
  component.add_authority_publisher(&publisher);
  component.setup();
  QC_CHECK(loop_to(component, ::quietcool::CoordinatorState::Idle, 2000));

  // One LOW command, confirmed by a 2-speed report (0x9F both halves).
  const auto round = [&]() {
    component.request_state(::quietcool::FanState::command(
        ::quietcool::Speed::Low, ::quietcool::Duration::Continuous));
    QC_CHECK(loop_to(component,
                     ::quietcool::CoordinatorState::PostCommandListening, 2000));
    const auto sent = radio.packets().back();
    QC_CHECK_EQ(sent.bytes[4], 0x9F);
    confirm_with(component, {sent.bytes[0], sent.bytes[1], sent.bytes[2],
                             sent.bytes[3], 0x9F, 0x9F});
    QC_CHECK(loop_to(component, ::quietcool::CoordinatorState::Idle, 2000));
  };

  // A warm-up round settles the one field a confirmed report legitimately
  // changes on a fresh boot — remembered_speed, which is deliberately
  // non-durable — so the counters below isolate the capability.
  round();
  const auto syncs = preferences.get().sync_count();
  const auto writes = preferences.get().write_count(kPreferenceKey);
  const auto publications = publisher.publications;

  for (int repeat = 0; repeat < 4; ++repeat) round();

  // The reports really were processed and really did re-confirm Two...
  QC_CHECK(publisher.publications > publications);
  QC_CHECK_EQ(publisher.last->speed_capability.value(), SpeedCapability::Two);
  // ...and none of them staged a byte or commanded a commit.
  QC_CHECK_EQ(preferences.get().write_count(kPreferenceKey), writes);
  QC_CHECK_EQ(preferences.get().sync_count(), syncs);
}

// Issue #31 review (opus-xhigh and codex): the band and the level are published
// from the same snapshot, so they cannot contradict each other.
//
// The trajectory, through the real component: a fan whose sticky capability is
// Three is commanded to HIGH (0xBF on the wire) and answers with that very
// byte. The frame is indistinguishable from our own echo, and its marker bits
// (10) alias SpeedCapability::Two — evidence ranking therefore refuses to let
// it demote the stored Three, which is correct. The two lines
// QuietCoolFan::publish_authority then runs must agree: clamping the level by
// the report's markers instead published 2 against a band of 3, so Home
// Assistant showed 2/3 (~66%) for a fan running at HIGH, and the next turn-on
// with no explicit speed reused that level and transmitted MED (0xAF).
QC_TEST("preferences", "an echo-confirmed HIGH publishes the top level, not MED") {
  ScopedPreferences preferences;
  {
    EspHomePreferencesAdapter writer(kPreferenceKey, 0);
    writer.load();
    QC_CHECK(provision(writer));
    QC_CHECK(save_capability(writer, SpeedCapability::Three));
  }

  host_test::set_millis(0);
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, 0, kPreferenceKey, 59);
  CapturingPublisher publisher;
  component.add_authority_publisher(&publisher);
  component.setup();
  FanSpeedBands bands;
  bands.observe(publisher.last->speed_capability);
  QC_CHECK_EQ(bands.entity(), 3);
  QC_CHECK_EQ(bands.command(), 3);

  QC_CHECK(loop_to(component, ::quietcool::CoordinatorState::Idle, 2000));
  component.request_state(
      ::quietcool::FanState::command(::quietcool::Speed::High,
                                     ::quietcool::Duration::Continuous));
  QC_CHECK(loop_to(component,
                   ::quietcool::CoordinatorState::PostCommandListening, 2000));
  const auto& sent = radio.packets().back();
  QC_CHECK_EQ(sent.bytes[4], 0xBF);
  confirm_with(component, {sent.bytes[0], sent.bytes[1], sent.bytes[2],
                           sent.bytes[3], 0xBF, 0xBF});

  // The echo did not demote the band...
  QC_CHECK_EQ(publisher.last->speed_capability.value(), SpeedCapability::Three);
  bands.observe(publisher.last->speed_capability);
  const auto count = bands.entity();
  QC_CHECK_EQ(count, 3);
  // ...and the confirmed frame it carries is the ambiguous 0xBF.
  const auto* confirmed =
      std::get_if<::quietcool::ConfirmedStateAuthority>(&publisher.last->state);
  QC_CHECK(confirmed != nullptr);
  QC_CHECK_EQ(confirmed->state.raw_byte(), 0xBF);
  QC_CHECK_EQ(confirmed->state.report_capability().value(),
              SpeedCapability::Two);  // the alias the mapping must ignore

  const auto feedback = authority_to_feedback(confirmed->state, count);
  QC_CHECK(feedback.on);
  QC_CHECK_EQ(feedback.speed, std::optional<int>(3));  // not 2 of 3

  // The downstream half: the entity caches that level, and a later turn-on
  // without an explicit speed re-commands HIGH rather than MED.
  QC_CHECK_EQ(fan_command_from_intent(true, *feedback.speed, bands.command())
                  .outbound_command_byte(),
              0xBF);
}

// Issue #31 review (opus-xhigh), through the real component: the trajectory a
// fresh 3-speed install actually takes. Nothing is provisioned with a
// capability, the entity is listed at whatever band setup() produces, and the
// fan then proves Three. Home Assistant is still working in the band it was
// listed at connect time, so ITS top-of-band press must still transmit HIGH —
// and the confirmed level it is shown must still fit that band.
//
// With one collapsed band the device listed 2, widened to 3 on learning, and
// Home Assistant's 100% (level 2) then mapped against 3 and transmitted MED
// (0xAF): the fan ran at MEDIUM while the UI read 100%, and HIGH was
// unreachable until the next reconnect or reboot.
QC_TEST("preferences", "learning three speeds cannot widen the band under Home Assistant") {
  ScopedPreferences preferences;
  {
    EspHomePreferencesAdapter writer(kPreferenceKey, 0);
    writer.load();
    QC_CHECK(provision(writer));  // provisioned, capability unknown
    QC_CHECK(!writer.load().speed_capability.has_value());
  }

  host_test::set_millis(0);
  ::quietcool::test::FakeRadio radio;
  QuietCoolComponent component(&radio, 0, kPreferenceKey, 59);
  CapturingPublisher publisher;
  component.add_authority_publisher(&publisher);
  component.setup();

  // What Home Assistant caches at ListEntities, before any RF evidence exists.
  FanSpeedBands bands;
  bands.observe(publisher.last->speed_capability);
  const int cached_band = bands.entity();

  // Its top-of-band press right now transmits HIGH...
  QC_CHECK(loop_to(component, ::quietcool::CoordinatorState::Idle, 2000));
  QC_CHECK_EQ(fan_command_from_intent(true, cached_band, bands.command())
                  .outbound_command_byte(),
              0xBF);
  component.request_state(fan_command_from_intent(true, cached_band,
                                                  bands.command()));
  QC_CHECK(loop_to(component,
                   ::quietcool::CoordinatorState::PostCommandListening, 2000));
  const auto& sent = radio.packets().back();
  QC_CHECK_EQ(sent.bytes[4], 0xBF);

  // ...and the fan answers with a genuine 3-speed report (marker bits 11, so
  // unambiguous: not our own echo of 0xBF), which teaches Three.
  confirm_with(component, {sent.bytes[0], sent.bytes[1], sent.bytes[2],
                           sent.bytes[3], 0xFF, 0xFF});
  QC_CHECK_EQ(publisher.last->speed_capability.value(), SpeedCapability::Three);
  bands.observe(publisher.last->speed_capability);

  // The listed band did not widen under the connection...
  QC_CHECK(bands.entity() <= cached_band);
  // ...the same press still transmits HIGH, never MED...
  QC_CHECK_EQ(fan_command_from_intent(true, cached_band, bands.command())
                  .outbound_command_byte(),
              0xBF);
  // ...and the level published for that confirmed HIGH still fits the band
  // Home Assistant is rendering it against.
  const auto* confirmed =
      std::get_if<::quietcool::ConfirmedStateAuthority>(&publisher.last->state);
  QC_CHECK(confirmed != nullptr);
  const auto feedback = authority_to_feedback(confirmed->state, bands.entity());
  QC_CHECK(feedback.speed.has_value());
  QC_CHECK(*feedback.speed <= cached_band);
  QC_CHECK_EQ(*feedback.speed, cached_band);  // a confirmed HIGH reads as 100%
}

}  // namespace
}  // namespace esphome::quietcool
