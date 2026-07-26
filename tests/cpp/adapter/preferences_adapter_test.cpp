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
  const auto count = authority_speed_count(*publisher.last, 3);
  QC_CHECK_EQ(count, 2);
  QC_CHECK_EQ(fan_command_from_intent(true, 2, count).outbound_command_byte(),
              0xBF);
}

}  // namespace
}  // namespace esphome::quietcool
