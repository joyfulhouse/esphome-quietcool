#pragma once

// Host stub. NaN is a meaningful published value here (it is how "timer
// remaining unknown" reaches Home Assistant), so it is recorded, not filtered.

#include <vector>

namespace esphome::sensor {

class Sensor {
 public:
  virtual ~Sensor() = default;

  void publish_state(float state) {
    state_ = state;
    has_state_ = true;
    published_.push_back(state);
  }

  float state() const { return state_; }
  bool has_state() const { return has_state_; }
  const std::vector<float>& published() const { return published_; }
  void clear_published() { published_.clear(); }

 private:
  float state_{0.0f};
  bool has_state_{false};
  std::vector<float> published_;
};

}  // namespace esphome::sensor
