#pragma once

#include "quietcool/core/confirmation_core.h"
#include "quietcool/ports/core_callback_queue.h"
#include "esp_event_sink.h"
#include "esp_monotonic_clock.h"
#include "preferences_adapter.h"
#include "quietcool/radio/burst_transmitter.h"

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"

#include <cstddef>
#include <cstdint>

namespace esphome::quietcool {

class AuthorityPublisher {
 public:
  virtual ~AuthorityPublisher() = default;
  virtual void publish_authority(
      const ::quietcool::AuthoritySnapshot& authority) = 0;
};

class QuietCoolComponent final : public Component {
 public:
  QuietCoolComponent(::quietcool::Radio* radio, std::uint32_t sender_seed,
                     std::uint32_t preference_key, std::uint32_t jitter_seed);

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override;

  // Four covers the fan, the timer select and headroom — same fixed-capacity,
  // no-heap discipline as CoreEffects::kCapacity. An overflow registration is
  // DROPPED rather than displacing a live publisher: silently unsubscribing
  // the fan entity would strand Home Assistant on stale state, which is worse
  // than refusing the newcomer.
  static constexpr std::size_t kMaxAuthorityPublishers = 4;

  void add_authority_publisher(AuthorityPublisher* publisher) {
    if (publisher == nullptr) return;
    if (authority_publisher_count_ >= kMaxAuthorityPublishers) return;
    authority_publishers_[authority_publisher_count_++] = publisher;
  }
  void set_state_known_sensor(binary_sensor::BinarySensor* sensor) {
    events_.set_state_known_sensor(sensor);
  }
  void set_timer_program_known_sensor(binary_sensor::BinarySensor* sensor) {
    events_.set_timer_program_known_sensor(sensor);
  }
  void set_timer_remaining_known_sensor(binary_sensor::BinarySensor* sensor) {
    events_.set_timer_remaining_known_sensor(sensor);
  }
  void set_confirmed_off_sensor(binary_sensor::BinarySensor* sensor) {
    events_.set_confirmed_off_sensor(sensor);
  }
  void set_controller_fault_sensor(binary_sensor::BinarySensor* sensor) {
    events_.set_controller_fault_sensor(sensor);
  }
  void set_timer_remaining_sensor(sensor::Sensor* sensor) {
    events_.set_timer_remaining_sensor(sensor);
  }
  void set_command_status_sensor(text_sensor::TextSensor* sensor) {
    events_.set_command_status_sensor(sensor);
  }
  void set_evidence_source_sensor(text_sensor::TextSensor* sensor) {
    events_.set_evidence_source_sensor(sensor);
  }
  void on_radio_packet(::quietcool::ByteView packet);
  void request_state(::quietcool::FanState requested);
  void request_manual_refresh();
  void request_learn(::quietcool::LearnMode mode);
  void request_forget();
  ::quietcool::CoreSnapshot snapshot() const;

#ifdef QUIETCOOL_HOST_TEST
  // Test seams (issue #9). Overflow of the exact-max effect and callback queues
  // is an invariant breach the real core cannot reach through the public
  // surface — a drain empties the effect queue before the next batch, and
  // callbacks drain one per loop — which is precisely why overflow is terminal.
  // The degradation contract is therefore exercised by forcing each queue to
  // reject the next admission at its real production site.
  void force_effect_queue_overflow_for_test() {
    ::quietcool::CoreEffects filler;
    filler.add(::quietcool::RequestRadioReset{0});
    while (effect_drain_.enqueue(filler, 0)) {
    }
    // enqueue_effects now rejects the batch and degrades; apply_effects returns
    // before draining.
    apply_effects(filler, 0);
  }
  void force_callback_queue_overflow_for_test() {
    while (core_callbacks_.enqueue(::quietcool::CoreCallbackKind::RadioRecovered,
                                   std::nullopt)) {
    }
    // apply_effect(RequestRadioReset) recovers the radio, then fails to enqueue
    // the RadioRecovered callback and degrades — the real line-152 path.
    apply_effect(::quietcool::RequestRadioReset{0});
  }
  // Fills the callback queue without applying anything (issue #22): a
  // subsequent RequestRadioReset in a drained batch then cannot enqueue its
  // RadioRecovered callback and degrades mid-drain.
  void fill_callback_queue_for_test() {
    while (core_callbacks_.enqueue(::quietcool::CoreCallbackKind::RadioRecovered,
                                   std::nullopt)) {
    }
  }
  // Drives a hand-built effect batch through the real apply_effects()/drain()
  // path (issue #22), so a degrade triggered by one effect is observed against
  // the effects that follow it in the same batch. Authority fan-out is
  // exercised through this same seam with a PublishAuthorityEffect batch,
  // rather than a dedicated publish hook.
  void drive_effects_for_test(const ::quietcool::CoreEffects& effects) {
    apply_effects(effects, 0);
  }
  // Degrades directly, without manipulating any queue (issue #23).
  void degrade_for_test() { degrade("test-forced degradation"); }
#endif

 private:
  void apply_effects(const ::quietcool::CoreEffects& effects,
                     ::quietcool::MonotonicMs now_ms);
  bool enqueue_effects(const ::quietcool::CoreEffects& effects,
                       ::quietcool::MonotonicMs now_ms);
  void apply_effect(const ::quietcool::CoreEffect& effect);
  void apply_burst_event(const ::quietcool::BurstEvent& event,
                         ::quietcool::MonotonicMs now_ms);
  void degrade(const char* reason);
  void note_budget_exhaustion();

  EspMonotonicClock clock_;
  ::quietcool::Radio& radio_;
  EspHomeEventSink events_;
  EspHomePreferencesAdapter preferences_;
  ::quietcool::ConfirmationCore core_;
  ::quietcool::BurstTransmitter burst_;
  ::quietcool::CoreEffectDrain effect_drain_;
  ::quietcool::CoreCallbackQueue core_callbacks_;
  AuthorityPublisher* authority_publishers_[kMaxAuthorityPublishers]{};
  std::size_t authority_publisher_count_{0};
  // Terminal-degradation latch (M2, issue #9). Set before any degradation
  // publication so an on_value automation fired synchronously during degrade()
  // cannot re-enter the non-reentrant core through a public entry point.
  bool degraded_{false};
  // Effect-drain budget-exhaustion diagnostic bookkeeping (M1, issue #8). A
  // one-shot latch keeps a sustained storm from logging every loop.
  std::uint32_t last_budget_exhaustions_{0};
  bool budget_warning_active_{false};
};

}  // namespace esphome::quietcool
