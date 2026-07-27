#pragma once

#include "quietcool/core/authority_store.h"

#include "esphome/core/preferences.h"

#include <cstddef>
#include <cstdint>

namespace esphome::quietcool {

class EspHomePreferencesAdapter final {
 public:
  EspHomePreferencesAdapter(std::uint32_t preference_key,
                            std::uint32_t compiled_sender_seed)
      : preference_key_(preference_key),
        compiled_sender_seed_(compiled_sender_seed) {}

  ::quietcool::RestorableState load();
  bool apply(const ::quietcool::PersistenceRequest& request);

 private:
  struct StoredRecord final {
    std::uint32_t magic;
    std::uint16_t adapter_schema_version;
    std::uint16_t core_schema_version;
    std::uint8_t flags;
    std::uint8_t seed_policy;
    std::uint8_t remembered_speed;
    // Repurposed reserved byte (issue #31); gated by kHasSpeedCapability, so
    // the layout and schema version are unchanged. Old firmware ignores the
    // unknown flag bit, and — critically — a schema-version bump would make a
    // ROLLBACK read the record as not intact and boot fail-closed into
    // SuppressCompiledSeed, i.e. unprovisioned. Never bump for this field.
    std::uint8_t speed_capability;
    std::uint32_t sender_be;
    std::uint32_t checksum;
  };
  // Layout pin: the record is persisted as raw bytes, so any size or offset
  // drift silently orphans every fielded unit's NVS record.
  static_assert(sizeof(StoredRecord) == 20, "StoredRecord layout is persisted");
  static_assert(offsetof(StoredRecord, flags) == 8 &&
                    offsetof(StoredRecord, speed_capability) == 11 &&
                    offsetof(StoredRecord, sender_be) == 12 &&
                    offsetof(StoredRecord, checksum) == 16,
                "StoredRecord layout is persisted");

  static std::uint32_t checksum(const StoredRecord& record);
  static bool intact(const StoredRecord& record);
  static StoredRecord encode(const ::quietcool::RestorableState& restored);
  static ::quietcool::RestorableState safe_suppressed_state();
  void apply_compiled_seed(::quietcool::RestorableState& restored) const;
  bool save_if_changed(bool durable);

  std::uint32_t preference_key_;
  std::uint32_t compiled_sender_seed_;
  ESPPreferenceObject preference_;
  ::quietcool::RestorableState restored_;
  StoredRecord stored_{};
  bool preference_ready_{false};
  bool stored_record_known_{false};
};

}  // namespace esphome::quietcool
