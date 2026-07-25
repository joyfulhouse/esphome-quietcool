#pragma once

#include "esphome/components/quietcool/core/authority_store.h"
#include "esphome/components/quietcool/ports/event_sink.h"

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <array>
#include <cstddef>
#include <optional>

namespace esphome::quietcool {

class EspHomeEventSink final : public ::quietcool::EventSink {
 public:
  void set_now_ms(::quietcool::MonotonicMs now_ms) { now_ms_ = now_ms; }
  void set_state_known_sensor(binary_sensor::BinarySensor* sensor) {
    state_known_ = sensor;
  }
  void set_timer_program_known_sensor(binary_sensor::BinarySensor* sensor) {
    timer_program_known_ = sensor;
  }
  void set_timer_remaining_known_sensor(binary_sensor::BinarySensor* sensor) {
    timer_remaining_known_ = sensor;
  }
  void set_confirmed_off_sensor(binary_sensor::BinarySensor* sensor) {
    confirmed_off_ = sensor;
  }
  void set_timer_remaining_sensor(sensor::Sensor* sensor) {
    timer_remaining_ = sensor;
  }
  void set_command_status_sensor(text_sensor::TextSensor* sensor) {
    command_status_ = sensor;
  }
  void set_evidence_source_sensor(text_sensor::TextSensor* sensor) {
    evidence_source_ = sensor;
  }

  void publish_authority(const ::quietcool::AuthoritySnapshot& authority,
                         ::quietcool::MonotonicMs now_ms);
  void on_core_event(const ::quietcool::CoreEvent& event) override;

 private:
  bool should_log(const ::quietcool::CoreEvent& event);
  void publish_command_status(const char* status);

  static constexpr std::size_t kEventKindCount = 12;
  std::array<::quietcool::MonotonicMs, kEventKindCount> last_log_ms_{};
  std::array<bool, kEventKindCount> logged_{};
  binary_sensor::BinarySensor* state_known_{nullptr};
  binary_sensor::BinarySensor* timer_program_known_{nullptr};
  binary_sensor::BinarySensor* timer_remaining_known_{nullptr};
  binary_sensor::BinarySensor* confirmed_off_{nullptr};
  sensor::Sensor* timer_remaining_{nullptr};
  text_sensor::TextSensor* command_status_{nullptr};
  text_sensor::TextSensor* evidence_source_{nullptr};
  std::optional<std::uint64_t> published_authority_revision_;
  ::quietcool::MonotonicMs last_timer_publish_ms_{0};
  bool timer_remaining_published_{false};
  bool command_status_published_{false};
  ::quietcool::MonotonicMs now_ms_{0};
};

}  // namespace esphome::quietcool
