#pragma once

// Host stub.

#include <string>
#include <vector>

namespace esphome::text_sensor {

class TextSensor {
 public:
  virtual ~TextSensor() = default;

  // Virtual like the binary-sensor stub's: observer tests override it to
  // assert mid-batch ordering (round 10).
  virtual void publish_state(const std::string& state) {
    state_ = state;
    has_state_ = true;
    published_.push_back(state);
  }

  const std::string& state() const { return state_; }
  bool has_state() const { return has_state_; }
  const std::vector<std::string>& published() const { return published_; }
  void clear_published() { published_.clear(); }

 private:
  std::string state_;
  bool has_state_{false};
  std::vector<std::string> published_;
};

}  // namespace esphome::text_sensor
