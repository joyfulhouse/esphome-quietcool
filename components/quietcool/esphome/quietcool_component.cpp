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
  if (degraded_) return;
  const auto now_ms = clock_.now_ms();
  if (const auto callback = core_callbacks_.pop(now_ms)) {
    if (callback->kind == ::quietcool::CoreCallbackKind::TxRejected &&
        callback->token) {
      apply_effects(core_.on_tx_rejected(*callback->token, callback->now_ms),
                    callback->now_ms);
    } else if (callback->kind ==
               ::quietcool::CoreCallbackKind::RadioRecovered) {
      apply_effects(core_.on_radio_recovered(callback->now_ms),
                    callback->now_ms);
    }
    return;
  }
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

// The degraded_ guards below make the non-reentrant core unreachable once the
// controller is terminally degraded (M2, issue #9). This closes the reentrancy
// the degradation publications open: publish_controller_failed() calls
// publish_state(), which can fire a user on_value automation synchronously, and
// that automation may call any of these public methods. Because degraded_ is
// set before those publications, every such call returns here.
void QuietCoolComponent::on_radio_packet(::quietcool::ByteView packet) {
  if (degraded_) return;
  const auto now_ms = clock_.now_ms();
  apply_effects(core_.on_frame(packet, now_ms), now_ms);
}

void QuietCoolComponent::request_state(::quietcool::FanState requested) {
  if (degraded_) return;
  const auto now_ms = clock_.now_ms();
  apply_effects(core_.request_state(requested, now_ms), now_ms);
}

void QuietCoolComponent::request_manual_refresh() {
  if (degraded_) return;
  const auto now_ms = clock_.now_ms();
  apply_effects(core_.request_manual_refresh(now_ms), now_ms);
}

void QuietCoolComponent::request_learn(::quietcool::LearnMode mode) {
  if (degraded_) return;
  const auto now_ms = clock_.now_ms();
  apply_effects(core_.request_learn(mode, now_ms), now_ms);
}

void QuietCoolComponent::request_forget() {
  if (degraded_) return;
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
  if (!enqueue_effects(effects, now_ms)) return;
  auto apply = [this](const ::quietcool::CoreEffect& effect,
                      ::quietcool::MonotonicMs effect_ms) {
    events_.set_now_ms(effect_ms);
    apply_effect(effect);
  };
  auto publish = [this](::quietcool::MonotonicMs publish_ms) {
    events_.publish_authority(core_.snapshot(publish_ms).authority, publish_ms);
  };
  // Only the top-level drain runs work and can exhaust its budget; a re-entrant
  // apply_effects (an automation firing during apply()) hits the draining_
  // guard and returns without draining, so it must not touch the diagnostic.
  const bool top_level = !effect_drain_.draining();
  effect_drain_.drain(apply, publish);
  if (top_level) note_budget_exhaustion();
}

// Budget exhaustion is normal backpressure (M1, issue #8), not a fault: it is
// logged rate-limited and never degrades the component. A one-shot latch emits
// one warning per storm rather than one per loop.
void QuietCoolComponent::note_budget_exhaustion() {
  const auto exhaustions = effect_drain_.budget_exhaustions();
  const bool newly_exhausted = exhaustions != last_budget_exhaustions_;
  last_budget_exhaustions_ = exhaustions;
  if (newly_exhausted && !budget_warning_active_)
    ESP_LOGW(kTag, "Effect drain budget hit; deferring effects to next loop");
  budget_warning_active_ = newly_exhausted;
}

bool QuietCoolComponent::enqueue_effects(
    const ::quietcool::CoreEffects& effects,
    ::quietcool::MonotonicMs now_ms) {
  if (effect_drain_.enqueue(effects, now_ms)) return true;
  degrade("core effect queue capacity exceeded");
  return false;
}

// Terminal degradation (M2, issue #9). Overflow means an exact-max queue
// invariant was breached; the core is non-reentrant and, at the effect-overflow
// site, mid-drain, so in-place recovery is unsafe. Stay terminal, but make the
// failure safe and visible: latch first (so synchronous automations fired by
// the publications below are refused at every public entry point), invalidate
// authority at the entity layer only, then keep mark_failed() for its
// reboot-required semantics.
void QuietCoolComponent::degrade(const char* reason) {
  if (degraded_) return;
  degraded_ = true;
  ESP_LOGE(kTag, "QuietCool controller degraded: %s; control and observation halted",
           reason);
  events_.publish_controller_failed();
  mark_failed();
}

void QuietCoolComponent::apply_effect(
    const ::quietcool::CoreEffect& effect) {
  if (const auto* request =
          std::get_if<::quietcool::RequestTxBurst>(&effect)) {
    if (burst_.accept(request->request) == ::quietcool::TxAcceptResult::Busy &&
        !core_callbacks_.enqueue(::quietcool::CoreCallbackKind::TxRejected,
                                 request->request.token)) {
      degrade("core callback queue capacity exceeded (tx rejected)");
    }
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
    if (radio_.recover() == ::quietcool::RadioRecoveryResult::Recovered &&
        !core_callbacks_.enqueue(::quietcool::CoreCallbackKind::RadioRecovered,
                                 std::nullopt)) {
      degrade("core callback queue capacity exceeded (radio recovered)");
    }
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
