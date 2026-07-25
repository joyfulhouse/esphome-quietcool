#pragma once

// Host stub. Records published values so tests can assert what Home Assistant
// would have been told.

#include <vector>

namespace esphome::binary_sensor {

class BinarySensor {
 public:
  virtual ~BinarySensor() = default;

  void publish_state(bool state) {
    state_ = state;
    has_state_ = true;
    published_.push_back(state);
  }

  bool state() const { return state_; }
  bool has_state() const { return has_state_; }
  const std::vector<bool>& published() const { return published_; }
  void clear_published() { published_.clear(); }

 private:
  bool state_{false};
  bool has_state_{false};
  std::vector<bool> published_;
};

}  // namespace esphome::binary_sensor
