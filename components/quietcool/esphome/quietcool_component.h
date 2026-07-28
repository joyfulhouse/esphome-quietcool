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
#include "esphome/core/log.h"

#include <cstddef>
#include <cstdint>
#include <optional>

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
    if (authority_publisher_count_ >= kMaxAuthorityPublishers) {
      // Loudly, not silently (round 1, opus): the dropped entity keeps its
      // controller_ pointer and can still TRANSMIT — it just never receives a
      // snapshot, so e.g. a dropped timer select would aim every command with
      // no confirmed state and nothing anywhere would say why.
      ESP_LOGE("quietcool", "authority publisher dropped: capacity %u exceeded",
               static_cast<unsigned>(kMaxAuthorityPublishers));
      return;
    }
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
  // Restored diagnostics (Task 4, issue #28's dropped entities). These five
  // live directly on the component rather than on EspHomeEventSink: their
  // sources (the echo-guard TX byte, the last accepted RX frame,
  // provisioned_sender_) are adapter-local observations events_ never sees.
  void set_last_tx_command_sensor(text_sensor::TextSensor* sensor) {
    last_tx_command_sensor_ = sensor;
  }
  void set_last_rx_frame_sensor(text_sensor::TextSensor* sensor) {
    last_rx_frame_sensor_ = sensor;
  }
  void set_last_confirmed_state_sensor(text_sensor::TextSensor* sensor) {
    last_confirmed_state_sensor_ = sensor;
  }
  void set_speed_capability_sensor(text_sensor::TextSensor* sensor) {
    speed_capability_sensor_ = sensor;
  }
  void set_remote_sender_id_sensor(text_sensor::TextSensor* sensor) {
    remote_sender_id_sensor_ = sensor;
  }
  // The three counters restored in Task 3 (tx_count() etc.); this task only
  // adds their entities and publication.
  void set_tx_count_sensor(sensor::Sensor* sensor) { tx_count_sensor_ = sensor; }
  void set_rx_valid_count_sensor(sensor::Sensor* sensor) {
    rx_valid_count_sensor_ = sensor;
  }
  void set_rx_rejected_count_sensor(sensor::Sensor* sensor) {
    rx_rejected_count_sensor_ = sensor;
  }
  void on_radio_packet(::quietcool::ByteView packet);
  void request_state(::quietcool::FanState requested);
  void request_manual_refresh();
  void request_learn(::quietcool::LearnMode mode);
  void request_forget();
  ::quietcool::CoreSnapshot snapshot() const;

  // Monotonic radio diagnostics, restored after the YAML->C++ migration
  // dropped them: never reset outside a reboot. `tx_count_` is the instrument
  // that once proved this bridge was not jamming the OEM remote — it stayed
  // flat through the remote's retry storm, which showed the storm was not
  // this bridge's own traffic. Losing it removed the evidence.
  std::uint32_t tx_count() const { return tx_count_; }
  std::uint32_t rx_valid_count() const { return rx_valid_count_; }
  std::uint32_t rx_rejected_count() const { return rx_rejected_count_; }

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
  // Publishes Last Confirmed Fan State and Fan Speed Capability from the
  // authority snapshot the PublishAuthorityEffect branch of apply_effect()
  // already carries — the same site every other AuthorityPublisher is fanned
  // out from, and the only site both fields' sources are confirmed-only.
  void publish_authority_diagnostics(
      const ::quietcool::AuthoritySnapshot& authority);
  // Publishes Remote Sender ID from provisioned_sender_. Called at exactly
  // the three sites provisioned_sender_ itself changes (see its declaration).
  void publish_remote_sender_id();

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
  // Radio diagnostics (see radio_counters_test.cpp and the task-3 report).
  // rx_valid_count_ / rx_rejected_count_ are FRAME-VALIDATION counters — did
  // the frame decode against the provisioned sender — not classifier-
  // relevance counters: ConfirmationCore::on_frame's accept/reject verdict
  // is not recoverable from its returned CoreEffects without misclassifying
  // the common case (a still-accumulating consensus candidate legitimately
  // returns empty effects), and that verdict is core-private regardless.
  std::uint32_t tx_count_{0};
  std::uint32_t rx_valid_count_{0};
  std::uint32_t rx_rejected_count_{0};
  // Last TX Command's latch: set when a TransactionCommand burst is ACCEPTED,
  // published (and cleared) when its burst COMPLETES, so the diagnostic can
  // never name a byte that a radio fault kept off the air (round 1, codex).
  std::optional<std::uint8_t> pending_tx_command_byte_;
  // Throttle for the rejected-counter's entity updates; the counter itself is
  // exact. 5 s bounds native-API traffic in a noisy RF environment.
  static constexpr ::quietcool::MonotonicMs kRejectedPublishIntervalMs = 5000;
  ::quietcool::MonotonicMs last_rejected_publish_ms_{0};
  // Mirrors ConfirmationCore's own sender_, so on_radio_packet can validate
  // an incoming frame the same way core eventually will, without reaching
  // into core. Updated at exactly the three sites where core's sender_
  // itself changes: restore() (setup()), Forget's EraseProvisioning, and
  // Learn's SaveProvisioning (see quietcool_component.cpp) — confirmed
  // against confirmation_core.cpp, where those are the only three sender_
  // assignments. Used by the RX counters above and by Remote Sender ID below.
  std::optional<::quietcool::SenderId> provisioned_sender_;
  // Restored diagnostics (Task 4). Last TX Command is published only for
  // TxReason::TransactionCommand (never a query burst) at the same
  // RequestTxBurst acceptance site that feeds the capability echo guard
  // (issue #31) — see confirmation_reducer.cpp's own_outbound_byte. Last
  // Valid RX Frame is published alongside rx_valid_count_. Neither byte is
  // latched as state here: each is read directly off the effect/packet that
  // is already in hand at its one publication site.
  text_sensor::TextSensor* last_tx_command_sensor_{nullptr};
  text_sensor::TextSensor* last_rx_frame_sensor_{nullptr};
  text_sensor::TextSensor* last_confirmed_state_sensor_{nullptr};
  text_sensor::TextSensor* speed_capability_sensor_{nullptr};
  text_sensor::TextSensor* remote_sender_id_sensor_{nullptr};
  sensor::Sensor* tx_count_sensor_{nullptr};
  sensor::Sensor* rx_valid_count_sensor_{nullptr};
  sensor::Sensor* rx_rejected_count_sensor_{nullptr};
};

}  // namespace esphome::quietcool
