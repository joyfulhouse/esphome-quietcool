#pragma once

// Host stub: in-memory NVS. Models the parts the preferences adapter uses —
// typed make_preference, save/load round-tripping, and an explicit sync — plus
// the failure modes it must survive (absent backend, empty store, corruption).
//
// It also models the RAM STAGE that makes sync() matter on real hardware.
// ESPHome's ESP32 backend does not write flash from ESPPreferenceObject::save;
// it stages the bytes in a vector (readable straight back, which is why save
// here is immediately visible to load) and only nvs_commit()s them from
// sync(), which the preferences component's interval syncer calls on a default
// 60 s flash_write_interval. simulate_power_loss() is the other half of that
// model: it drops everything staged since the last sync, i.e. what an
// ungraceful power cut takes with it. Without it a test cannot tell a durable
// save from a staged one, because both round-trip in RAM.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace esphome {

class ESPPreferenceBackend {
 public:
  virtual ~ESPPreferenceBackend() = default;
  virtual bool save(const std::uint8_t* data, std::size_t len) = 0;
  virtual bool load(std::uint8_t* data, std::size_t len) = 0;
};

class ESPPreferenceObject {
 public:
  ESPPreferenceObject() = default;
  explicit ESPPreferenceObject(ESPPreferenceBackend* backend)
      : backend_(backend) {}

  template <typename T>
  bool save(const T* src) {
    if (backend_ == nullptr) return false;
    return backend_->save(reinterpret_cast<const std::uint8_t*>(src), sizeof(T));
  }

  template <typename T>
  bool load(T* dest) {
    if (backend_ == nullptr) return false;
    return backend_->load(reinterpret_cast<std::uint8_t*>(dest), sizeof(T));
  }

 private:
  ESPPreferenceBackend* backend_{nullptr};
};

class ESPPreferences {
 public:
  virtual ~ESPPreferences() = default;

  template <typename T>
  ESPPreferenceObject make_preference(std::uint32_t type, bool /*in_flash*/ = false) {
    return ESPPreferenceObject(&backend_for(type, sizeof(T)));
  }

  virtual bool sync() {
    ++sync_count_;
    if (!sync_result_) return false;
    for (auto& entry : records_) entry.second.commit();
    return true;
  }

  // Test controls.
  void set_sync_result(bool result) { sync_result_ = result; }
  // Commits ATTEMPTED, including ones set_sync_result(false) made fail.
  std::size_t sync_count() const { return sync_count_; }
  // How many times the adapter handed bytes to the backend, committed or not.
  // Pins the other half of "no write when unchanged": a save that is merely
  // staged still costs RAM and still ends up in flash at the next sync.
  std::size_t write_count(std::uint32_t type) const {
    const auto it = records_.find(type);
    return it == records_.end() ? 0 : it->second.writes;
  }
  // An ungraceful power cut: everything staged since the last successful sync
  // is gone, and the device comes back up reading the last COMMITTED bytes.
  void simulate_power_loss() {
    for (auto& entry : records_) entry.second.roll_back();
  }
  void clear() { records_.clear(); }
  // Simulates a corrupt record: right size, wrong bytes.
  void corrupt(std::uint32_t type) {
    auto it = records_.find(type);
    if (it != records_.end())
      std::memset(it->second.bytes.data(), 0xA5, it->second.bytes.size());
  }
  bool has_record(std::uint32_t type) const {
    return records_.find(type) != records_.end();
  }

 private:
  struct Record final : ESPPreferenceBackend {
    std::vector<std::uint8_t> bytes;   // the RAM stage, readable immediately
    std::vector<std::uint8_t> flashed; // what survives a power cut
    bool written{false};
    bool flashed_written{false};
    std::size_t writes{0};

    bool save(const std::uint8_t* data, std::size_t len) override {
      bytes.assign(data, data + len);
      written = true;
      ++writes;
      return true;
    }
    bool load(std::uint8_t* data, std::size_t len) override {
      if (!written || bytes.size() != len) return false;
      std::memcpy(data, bytes.data(), len);
      return true;
    }
    void commit() {
      flashed = bytes;
      flashed_written = written;
    }
    void roll_back() {
      bytes = flashed;
      written = flashed_written;
    }
  };

  Record& backend_for(std::uint32_t type, std::size_t size) {
    auto& record = records_[type];
    if (record.bytes.empty()) record.bytes.resize(size);
    return record;
  }

  std::map<std::uint32_t, Record> records_;
  bool sync_result_{true};
  std::size_t sync_count_{0};
};

extern ESPPreferences* global_preferences;

}  // namespace esphome
