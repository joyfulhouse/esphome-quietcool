#pragma once

#include "quietcool/core/authority_store.h"

#include "esphome/core/preferences.h"

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
    std::uint8_t reserved;
    std::uint32_t sender_be;
    std::uint32_t checksum;
  };

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
