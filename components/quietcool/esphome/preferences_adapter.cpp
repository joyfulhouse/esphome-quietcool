#include "preferences_adapter.h"

#include <cstring>
#include <type_traits>

namespace esphome::quietcool {
namespace {

constexpr std::uint32_t kPreferenceMagic = 0x51435032U;
constexpr std::uint16_t kAdapterSchemaVersion = 1;
constexpr std::uint8_t kHasSender = 1U << 0U;
constexpr std::uint8_t kHasRememberedSpeed = 1U << 1U;
constexpr std::uint8_t kHasSpeedCapability = 1U << 2U;
constexpr std::uint32_t kFnvOffset = 2166136261U;
constexpr std::uint32_t kFnvPrime = 16777619U;

}  // namespace

std::uint32_t EspHomePreferencesAdapter::checksum(const StoredRecord& record) {
  auto value = kFnvOffset;
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&record);
  constexpr auto length = sizeof(StoredRecord) - sizeof(record.checksum);
  for (std::size_t index = 0; index < length; ++index) {
    value ^= bytes[index];
    value *= kFnvPrime;
  }
  return value;
}

bool EspHomePreferencesAdapter::intact(const StoredRecord& record) {
  return record.magic == kPreferenceMagic &&
         record.adapter_schema_version == kAdapterSchemaVersion &&
         record.checksum == checksum(record);
}

EspHomePreferencesAdapter::StoredRecord EspHomePreferencesAdapter::encode(
    const ::quietcool::RestorableState& restored) {
  StoredRecord record{};
  record.magic = kPreferenceMagic;
  record.adapter_schema_version = kAdapterSchemaVersion;
  record.core_schema_version = restored.version;
  record.seed_policy = static_cast<std::uint8_t>(restored.seed_policy);
  if (restored.sender) {
    record.flags |= kHasSender;
    record.sender_be = restored.sender->as_be_u32();
  }
  if (restored.remembered_speed) {
    record.flags |= kHasRememberedSpeed;
    record.remembered_speed =
        static_cast<std::uint8_t>(*restored.remembered_speed);
  }
  if (restored.speed_capability) {
    record.flags |= kHasSpeedCapability;
    record.speed_capability =
        static_cast<std::uint8_t>(*restored.speed_capability);
  }
  record.checksum = checksum(record);
  return record;
}

::quietcool::RestorableState
EspHomePreferencesAdapter::safe_suppressed_state() {
  ::quietcool::RestorableState restored;
  restored.seed_policy = ::quietcool::SeedPolicy::SuppressCompiledSeed;
  return restored;
}

void EspHomePreferencesAdapter::apply_compiled_seed(
    ::quietcool::RestorableState& restored) const {
  if (restored.sender ||
      restored.seed_policy != ::quietcool::SeedPolicy::AllowCompiledSeed ||
      compiled_sender_seed_ == 0)
    return;
  const auto sender =
      ::quietcool::SenderId::from_be_u32(compiled_sender_seed_);
  if (sender) restored.sender = sender.value();
}

::quietcool::RestorableState EspHomePreferencesAdapter::load() {
  static_assert(std::is_trivially_copyable<StoredRecord>::value,
                "ESPHome preferences require a trivially copyable record");
  if (global_preferences == nullptr) {
    restored_ = safe_suppressed_state();
    return restored_;
  }
  preference_ =
      global_preferences->make_preference<StoredRecord>(preference_key_, true);
  preference_ready_ = true;
  StoredRecord record{};
  if (!preference_.load(&record)) {
    restored_ = {};
    apply_compiled_seed(restored_);
    return restored_;
  }
  if (!intact(record)) {
    restored_ = safe_suppressed_state();
    return restored_;
  }

  ::quietcool::RestorableState candidate;
  candidate.version = record.core_schema_version;
  candidate.seed_policy =
      static_cast<::quietcool::SeedPolicy>(record.seed_policy);
  if ((record.flags & kHasSender) != 0) {
    const auto sender = ::quietcool::SenderId::from_be_u32(record.sender_be);
    if (!sender) candidate.version = 0;
    if (sender) candidate.sender = sender.value();
  }
  if ((record.flags & kHasRememberedSpeed) != 0)
    candidate.remembered_speed =
        static_cast<::quietcool::Speed>(record.remembered_speed);
  // A capability byte outside {1, 2, 3} fails restorable_state_is_valid below,
  // which fails the whole record closed into safe_suppressed_state.
  if ((record.flags & kHasSpeedCapability) != 0)
    candidate.speed_capability =
        static_cast<::quietcool::SpeedCapability>(record.speed_capability);
  if (!::quietcool::restorable_state_is_valid(candidate)) {
    restored_ = safe_suppressed_state();
    return restored_;
  }

  restored_ = candidate;
  stored_ = record;
  stored_record_known_ = true;
  apply_compiled_seed(restored_);
  return restored_;
}

bool EspHomePreferencesAdapter::apply(
    const ::quietcool::PersistenceRequest& request) {
  bool durable = false;
  switch (request.kind) {
    case ::quietcool::PersistenceKind::SaveProvisioning:
      if (!request.sender) return false;
      restored_.sender = request.sender;
      restored_.seed_policy = ::quietcool::SeedPolicy::AllowCompiledSeed;
      // Provisioning re-binds the record to a fan, and the capability is a
      // property of that fan: take whatever the core's sticky value is NOW
      // (nullopt after learning a different fan, the retained value after
      // re-learning the same one). Keeping the previous restored_ value here
      // re-persisted the OLD fan's capability under the NEW sender, which
      // then survived reboots and mis-aimed commands (issue #31).
      restored_.speed_capability = request.speed_capability;
      durable = true;
      break;
    case ::quietcool::PersistenceKind::EraseProvisioning:
      restored_ = safe_suppressed_state();
      durable = true;
      break;
    case ::quietcool::PersistenceKind::SaveRememberedSpeed:
      if (!request.remembered_speed) return false;
      restored_.remembered_speed = request.remembered_speed;
      break;
    case ::quietcool::PersistenceKind::SaveSpeedCapability:
      // Durable, like SaveProvisioning. ESPHome's ESP32 backend does not write
      // flash from ESPPreferenceObject::save; it STAGES the bytes in RAM, and
      // nothing reaches flash until sync(), which the preferences component's
      // interval syncer calls on a default 60 s flash_write_interval. A
      // capability learned seconds after boot would otherwise be lost to an
      // ungraceful power cut — the exact event issue #31 exists to survive —
      // and losing it reopens the unknown-capability window on a fan whose
      // band was already proven.
      //
      // This is NOT a flash write per confirmed report. Every report confirms
      // the capability, but two independent filters stand in the way: the core
      // emits this request only when the sticky capability CHANGES
      // (ConfirmationCore::promote_authority), and save_if_changed below
      // commits only when the ENCODED RECORD changes, so a report agreeing
      // with what is already stored costs nothing even if that first filter
      // regresses. Both halves are pinned by the adapter suite.
      //
      // EraseProvisioning above resets restored_ wholesale, so Forget also
      // erases the capability — a stale capability must not survive
      // re-learning a different fan.
      if (!request.speed_capability) return false;
      restored_.speed_capability = request.speed_capability;
      durable = true;
      break;
  }
  return ::quietcool::restorable_state_is_valid(restored_) &&
         save_if_changed(durable);
}

bool EspHomePreferencesAdapter::save_if_changed(bool durable) {
  if (!preference_ready_ || global_preferences == nullptr) return false;
  const auto record = encode(restored_);
  const bool changed =
      !stored_record_known_ ||
      std::memcmp(&stored_, &record, sizeof(StoredRecord)) != 0;
  if (changed) {
    if (!preference_.save(&record)) return false;
    stored_ = record;
    stored_record_known_ = true;
    if (durable) durable_commit_pending_ = true;
  }
  // sync() commits the whole NVS handle, so calling it for a request that
  // staged nothing would put a flash erase/write cycle behind a no-op — and
  // SaveSpeedCapability, now durable, is re-asserted for as long as the fan
  // keeps reporting. Gating on durable_commit_pending_ rather than on
  // `durable` alone makes "a flash commit happens only when the stored record
  // actually changed" true HERE, independently of the core's own change
  // filter.
  //
  // It is the pending FLAG and not `changed` that gates it, because a sync()
  // may fail: the record is staged and stored_ already matches it, so an
  // identical durable request would otherwise find nothing changed and report
  // success over a value still living only in RAM. The flag keeps the retry.
  if (!durable || !durable_commit_pending_) return true;
  if (!global_preferences->sync()) return false;
  durable_commit_pending_ = false;
  return true;
}

}  // namespace esphome::quietcool
