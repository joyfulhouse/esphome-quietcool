#include "quietcool_component.h"

#include "esphome/core/log.h"

#include <variant>

namespace esphome::quietcool {
namespace {

constexpr char kTag[] = "quietcool";

}  // namespace

QuietCoolComponent::QuietCoolComponent(
    ::quietcool::Radio* radio, std::uint32_t sender_seed,
    std::uint32_t preference_key, std::uint32_t jitter_seed)
    : radio_(*radio),
      preferences_(preference_key, sender_seed),
      core_(::quietcool::CoreConfig{jitter_seed}),
      burst_(clock_, radio_) {}

float QuietCoolComponent::get_setup_priority() const {
  return setup_priority::LATE;
}

void QuietCoolComponent::setup() {
  const auto now_ms = clock_.now_ms();
  apply_effects(core_.restore(preferences_.load(), now_ms), now_ms);
  apply_effects(core_.on_radio_ready(now_ms), now_ms);
}

void QuietCoolComponent::loop() {
  const auto now_ms = clock_.now_ms();
  if (const auto event = burst_.poll(now_ms)) {
    apply_burst_event(*event, now_ms);
    if (std::holds_alternative<::quietcool::BurstStarted>(*event)) {
      if (const auto send_event = burst_.poll(now_ms))
        apply_burst_event(*send_event, now_ms);
    }
    return;
  }
  apply_effects(core_.poll(now_ms), now_ms);
}

void QuietCoolComponent::dump_config() {
  ESP_LOGCONFIG(kTag, "QuietCool confirmation controller");
}

void QuietCoolComponent::on_radio_packet(::quietcool::ByteView packet) {
  const auto now_ms = clock_.now_ms();
  apply_effects(core_.on_frame(packet, now_ms), now_ms);
}

void QuietCoolComponent::request_state(::quietcool::FanState requested) {
  const auto now_ms = clock_.now_ms();
  apply_effects(core_.request_state(requested, now_ms), now_ms);
}

void QuietCoolComponent::request_manual_refresh() {
  const auto now_ms = clock_.now_ms();
  apply_effects(core_.request_manual_refresh(now_ms), now_ms);
}

void QuietCoolComponent::request_learn(::quietcool::LearnMode mode) {
  const auto now_ms = clock_.now_ms();
  apply_effects(core_.request_learn(mode, now_ms), now_ms);
}

void QuietCoolComponent::request_forget() {
  const auto now_ms = clock_.now_ms();
  apply_effects(core_.request_forget(now_ms), now_ms);
}

::quietcool::CoreSnapshot QuietCoolComponent::snapshot() const {
  const auto now_ms = clock_.now_ms();
  return core_.snapshot(now_ms);
}

void QuietCoolComponent::apply_effects(
    const ::quietcool::CoreEffects& effects,
    ::quietcool::MonotonicMs now_ms) {
  events_.set_now_ms(now_ms);
  for (std::size_t index = 0; index < effects.size(); ++index)
    apply_effect(effects[index], now_ms);
  events_.publish_authority(core_.snapshot(now_ms).authority, now_ms);
}

void QuietCoolComponent::apply_effect(
    const ::quietcool::CoreEffect& effect,
    ::quietcool::MonotonicMs now_ms) {
  if (const auto* request =
          std::get_if<::quietcool::RequestTxBurst>(&effect)) {
    if (burst_.accept(request->request) == ::quietcool::TxAcceptResult::Busy)
      apply_effects(core_.on_tx_rejected(request->request.token, now_ms),
                    now_ms);
    return;
  }
  if (const auto* revoke = std::get_if<::quietcool::RevokeTxLease>(&effect)) {
    burst_.revoke_if_unstarted(revoke->token);
    return;
  }
  if (const auto* event = std::get_if<::quietcool::PublishCoreEvent>(&effect)) {
    events_.on_core_event(event->event);
    return;
  }
  if (const auto* persistence =
          std::get_if<::quietcool::RequestPersistenceEffect>(&effect)) {
    if (!preferences_.apply(persistence->request))
      ESP_LOGW(kTag, "Unable to persist requested core state");
    return;
  }
  if (const auto* authority =
          std::get_if<::quietcool::PublishAuthorityEffect>(&effect)) {
    if (authority_publisher_ != nullptr)
      authority_publisher_->publish_authority(authority->authority);
    return;
  }
  if (std::holds_alternative<::quietcool::RequestRadioReset>(effect)) {
    if (radio_.recover() == ::quietcool::RadioRecoveryResult::Recovered)
      apply_effects(core_.on_radio_recovered(now_ms), now_ms);
    return;
  }
  if (const auto* refusal = std::get_if<::quietcool::RefusedInput>(&effect)) {
    events_.on_core_event({::quietcool::CoreEventKind::RequestRefused,
                           refusal->state, refusal->reason, std::nullopt,
                           std::nullopt});
  }
}

void QuietCoolComponent::apply_burst_event(
    const ::quietcool::BurstEvent& event,
    ::quietcool::MonotonicMs now_ms) {
  if (const auto* started = std::get_if<::quietcool::BurstStarted>(&event)) {
    apply_effects(core_.on_tx_started(started->token, now_ms), now_ms);
  } else if (const auto* complete =
                 std::get_if<::quietcool::BurstComplete>(&event)) {
    apply_effects(core_.on_tx_complete(complete->token, now_ms), now_ms);
  } else if (const auto* rejected =
                 std::get_if<::quietcool::BurstRejected>(&event)) {
    apply_effects(core_.on_tx_rejected(rejected->token, now_ms), now_ms);
  } else if (const auto* fault =
                 std::get_if<::quietcool::BurstFault>(&event)) {
    apply_effects(core_.on_tx_fault(fault->token, now_ms), now_ms);
  }
}

}  // namespace esphome::quietcool
