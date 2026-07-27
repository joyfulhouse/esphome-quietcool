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
// The last test drives the REAL component through the real adapter and core:
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
  component.set_authority_publisher(&publisher);
  component.setup();

  QC_CHECK(publisher.last.has_value());
  QC_CHECK(std::holds_alternative<::quietcool::UnknownStateAuthority>(
      publisher.last->state));
  QC_CHECK_EQ(publisher.last->speed_capability.value(), SpeedCapability::Two);

  // A level-2 press mapped against the seeded count transmits HIGH (0xBF).
  const auto count = authority_speed_count(*publisher.last);
  QC_CHECK_EQ(count, 2);
  QC_CHECK_EQ(fan_command_from_intent(true, 2, count).outbound_command_byte(),
              0xBF);
}

// The other half of the #31 chain, on a unit that has never learned a band:
// a 2-speed fan's confirmation must narrow the entity AND reach NVS, so the
// window this branch closes is entered at most once per fan. The fan's
// confirming report is byte-identical to the command frame (both carry marker
// bits 10), which is exactly the frame the echo filter cannot attribute — if
// that evidence is discarded rather than ranked, the count stays at the
// compiled 3 and a level-2 press transmits MED, which stops the fan (#30).
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
  component.set_authority_publisher(&publisher);
  component.setup();
  QC_CHECK_EQ(authority_speed_count(*publisher.last),
              kCompiledDefaultSpeedCount);

  const auto loop_to = [&](::quietcool::CoordinatorState target,
                           std::size_t limit) {
    for (std::size_t pass = 0; pass < limit; ++pass) {
      if (component.snapshot().state == target) return true;
      host_test::advance_millis(25);
      component.call_loop();
    }
    return component.snapshot().state == target;
  };

  // Let the unanswered boot query lapse, then command LOW.
  QC_CHECK(loop_to(::quietcool::CoordinatorState::Idle, 2000));
  component.request_state(
      ::quietcool::FanState::command(::quietcool::Speed::Low,
                                     ::quietcool::Duration::Continuous));
  QC_CHECK(loop_to(::quietcool::CoordinatorState::PostCommandListening, 2000));
  const auto& sent = radio.packets().back();
  QC_CHECK_EQ(sent.bytes[4], 0x9F);

  // Two independent confirmations inside the acceptance window, byte-identical
  // to what we transmitted — a genuine 2-speed report.
  const std::array<std::uint8_t, 6> report{sent.bytes[0], sent.bytes[1],
                                           sent.bytes[2], sent.bytes[3],
                                           0x9F,          0x9F};
  host_test::advance_millis(500);
  component.on_radio_packet(::quietcool::ByteView(report.data(), 6));
  host_test::advance_millis(100);
  component.on_radio_packet(::quietcool::ByteView(report.data(), 6));
  component.call_loop();

  QC_CHECK(publisher.last->speed_capability.has_value());
  QC_CHECK_EQ(publisher.last->speed_capability.value(), SpeedCapability::Two);
  const auto count = authority_speed_count(*publisher.last);
  QC_CHECK_EQ(count, 2);
  QC_CHECK_EQ(fan_command_from_intent(true, 2, count).outbound_command_byte(),
              0xBF);

  // And it is durable, so the next boot never reopens the window.
  EspHomePreferencesAdapter reader(kPreferenceKey, 0);
  const auto reloaded = reader.load().speed_capability;
  QC_CHECK(reloaded.has_value());
  QC_CHECK_EQ(reloaded.value(), SpeedCapability::Two);
}

}  // namespace
}  // namespace esphome::quietcool
