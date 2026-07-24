#pragma once

// Host stub: in-memory NVS. Models the parts the preferences adapter uses —
// typed make_preference, save/load round-tripping, and an explicit sync — plus
// the failure modes it must survive (absent backend, empty store, corruption).

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
    return sync_result_;
  }

  // Test controls.
  void set_sync_result(bool result) { sync_result_ = result; }
  std::size_t sync_count() const { return sync_count_; }
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
    std::vector<std::uint8_t> bytes;
    bool written{false};

    bool save(const std::uint8_t* data, std::size_t len) override {
      bytes.assign(data, data + len);
      written = true;
      return true;
    }
    bool load(std::uint8_t* data, std::size_t len) override {
      if (!written || bytes.size() != len) return false;
      std::memcpy(data, bytes.data(), len);
      return true;
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
