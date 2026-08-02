#include "quietcool_component.h"

#include "diagnostic_format.h"
#include "quietcool/core/frame_codec.h"

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
  // Guarded like every other public entry point (issue #23): a synchronous
  // automation fired by publish_controller_failed() can call setup(), which
  // would otherwise re-enter core_.restore() / core_.on_radio_ready() on a core
  // already declared broken. setup() legitimately runs once at boot, before any
  // degradation, when this latch is false — so normal startup is unaffected.
  if (degraded_) return;
  const auto now_ms = clock_.now_ms();
  const auto restored = preferences_.load();
  // Mirrors what core_.restore() below is about to adopt as its own sender_
  // (handle_restore: sender_ = valid ? restored.sender : nullopt) — load()
  // has already validated the record, so `restored` here is exactly what
  // core will treat as valid too. See provisioned_sender_'s declaration.
  provisioned_sender_ = restored.sender;
  apply_effects(core_.restore(restored, now_ms), now_ms);
  apply_effects(core_.on_radio_ready(now_ms), now_ms);
  // Diagnostics publish only AFTER the core is fully restored (round 1,
  // codex): publish_state can fire a synchronous on_value automation, and one
  // that calls Forget between our cache write and core_.restore() would clear
  // cache/NVS and then have restore() re-adopt the stale record — core bound
  // to sender A while the adapter believes itself unprovisioned. After
  // restore, a synchronous Forget goes through the core like any other
  // command. The initial counter zeros publish here for the same reason, and
  // because a counter that never publishes reads as Unknown forever on a
  // quiet-RF boot rather than as the true zero.
  publish_remote_sender_id();
  // The fault latch publishes its healthy state for the same reason the
  // counters publish their zeros: its only other writer is the degradation
  // path, so an unfaulted boot otherwise reads Unknown forever and a
  // problem-class sensor cannot say "no problem" (issue #35 batch, observed
  // on both production units). Safe at this point: degraded_ was checked at
  // entry, and a synchronous on_state automation here goes through the same
  // guarded public entry points as any other post-restore callback.
  events_.publish_controller_healthy();
  if (tx_count_sensor_ != nullptr) tx_count_sensor_->publish_state(tx_count_);
  if (rx_valid_count_sensor_ != nullptr)
    rx_valid_count_sensor_->publish_state(rx_valid_count_);
  if (rx_rejected_count_sensor_ != nullptr)
    rx_rejected_count_sensor_->publish_state(rx_rejected_count_);
}

void QuietCoolComponent::loop() {
  if (degraded_) return;
  const auto now_ms = clock_.now_ms();
  // Before the early returns, so a busy loop (callback/burst work every tick)
  // cannot starve the counter flush indefinitely.
  flush_rx_counter_publications(now_ms);
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
  // RX Valid/Rejected are frame-validation counters (does this frame decode
  // against the provisioned sender?), matching what the legacy YAML build
  // measured — NOT ConfirmationCore::on_frame's classifier-relevance verdict,
  // which is core-private and not safely recoverable from its returned
  // CoreEffects (see the task-3 report). FrameCodec::decode_strict is a pure
  // static function, so calling it here duplicates a cheap decode of a
  // ~10-byte frame rather than reaching into core's classification state;
  // core will decode the same bytes again inside on_frame() below. Do not
  // "optimise" this into sharing on_frame's internal decode result — that
  // would require exposing core-private state, which is the change this
  // adapter-only observation was chosen specifically to avoid.
  // Unprovisioned frames are NOT counted at all (round 1, fable + opus): with
  // no sender there is nothing to validate against, and the only unprovisioned
  // traffic that matters is a Learn window — where the frames the core is
  // ACCEPTING as learn evidence would have inflated RX Rejected precisely
  // while an operator is watching the diagnostics.
  //
  // A frame that decode_strict rejects but FrameRecovery later repairs into
  // consensus evidence still counts rejected here, deliberately: these are
  // frame-VALIDATION counters (design doc §3.5), and such a frame genuinely
  // failed strict validation. RX Rejected rising while commands still confirm
  // is the recovered-frames signature, not a contradiction.
  const auto now_ms = clock_.now_ms();
  if (provisioned_sender_) {
    const auto decoded =
        ::quietcool::FrameCodec::decode_strict(packet, *provisioned_sender_);
    if (decoded) {
      ++rx_valid_count_;
      // Only state-carrying decodes reach the "Last Valid RX Frame" text
      // sensor: an ExactQuery's marker byte is 0x66, not a fan state, and
      // publishing it made the diagnostic show a query marker every time the
      // OEM remote polled (round 1, opus). Change-gated: a retry storm
      // repeats one byte, so identical frames publish once (round 2).
      // packet.size() == 6 is proven by the successful decode (length is
      // decode_strict's first check).
      const bool carries_state =
          !std::holds_alternative<::quietcool::ExactQuery>(decoded.value());
      if (carries_state && published_rx_frame_byte_ != packet[4]) {
        published_rx_frame_byte_ = packet[4];
        pending_rx_frame_publish_ = packet[4];
      }
    } else {
      ++rx_rejected_count_;
    }
    // Counter ENTITY updates are deliberately absent here: both RX counters
    // publish from loop()'s paced flush (round 2, all three engines), so a
    // packet storm costs increments only — no per-packet native-API traffic,
    // and no stale final value once the storm ends.
  }
  // Core processes the frame BEFORE any diagnostic publishes (round 3,
  // codex): publish_state can fire a synchronous automation, and one that
  // commands the fan would otherwise run against the core's PRE-frame state —
  // e.g. a timer selected from an on_value automation racing the very
  // OEM-priority handling this frame is about to trigger.
  apply_effects(core_.on_frame(packet, now_ms), now_ms);
  if (pending_rx_frame_publish_) {
    if (last_rx_frame_sensor_ != nullptr)
      last_rx_frame_sensor_->publish_state(
          format_hex_byte(*pending_rx_frame_publish_));
    pending_rx_frame_publish_.reset();
  }
}

void QuietCoolComponent::flush_rx_counter_publications(
    ::quietcool::MonotonicMs now_ms) {
  if (rx_valid_count_ == published_rx_valid_count_ &&
      rx_rejected_count_ == published_rx_rejected_count_)
    return;
  if (last_rx_counter_publish_ms_ != 0 &&
      now_ms - last_rx_counter_publish_ms_ < kRxCounterPublishIntervalMs)
    return;
  last_rx_counter_publish_ms_ = now_ms;
  if (rx_valid_count_ != published_rx_valid_count_) {
    published_rx_valid_count_ = rx_valid_count_;
    if (rx_valid_count_sensor_ != nullptr)
      rx_valid_count_sensor_->publish_state(static_cast<float>(rx_valid_count_));
  }
  if (rx_rejected_count_ != published_rx_rejected_count_) {
    published_rx_rejected_count_ = rx_rejected_count_;
    if (rx_rejected_count_sensor_ != nullptr)
      rx_rejected_count_sensor_->publish_state(
          static_cast<float>(rx_rejected_count_));
  }
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
  // The learn ambiguity abort (issue #6) only protects pairing when the
  // intended fan is among the senders heard; a lone foreign fan binds
  // unopposed (issue #17), so the operator must keep the target transmitting.
  ESP_LOGW(kTag,
           "Learn window opening: operate the target fan's own remote for the "
           "whole window so the intended fan is among the senders heard");
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
    // Entity fan-out rides the SAME post-drain channel as the *_Known
    // sensors (round 2, codex high): several authority invalidations — the
    // estimated-timer-deadline path among them — never emit an explicit
    // PublishAuthorityEffect, so an effect-driven fan-out silently missed
    // exactly the invalidations the timer select's cache rule exists to see.
    const auto authority = core_.snapshot(publish_ms).authority;
    // Same staged delivery as the in-place channel (rounds 10-11).
    deliver_authority(authority, publish_ms);
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
  // Stop the drain terminally (issue #22). degrade() is reached from inside
  // apply_effect(), i.e. from within CoreEffectDrain::drain(); without this the
  // drain would keep applying the rest of the batch after we return, and a
  // later RequestAccepted / authority / persistence effect would overrun the
  // terminal "unavailable" publication below. This is NOT the backpressure
  // path — budget exhaustion (issue #8) must keep draining with nothing dropped.
  effect_drain_.halt();
  ESP_LOGE(kTag, "QuietCool controller degraded: %s; control and observation halted",
           reason);
  events_.publish_controller_failed();
  mark_failed();
}

void QuietCoolComponent::apply_effect(
    const ::quietcool::CoreEffect& effect) {
  if (const auto* request =
          std::get_if<::quietcool::RequestTxBurst>(&effect)) {
    const auto accept_result = burst_.accept(request->request);
    // Last TX Command: LATCHED here for a real state command (never a query
    // burst — the same restriction confirmation_reducer.cpp applies to the
    // capability echo guard's own_outbound_byte), but PUBLISHED only at
    // BurstComplete. Publishing at acceptance claimed a transmission that a
    // radio fault could still prevent, so the diagnostic could name a byte
    // that never went on air while TX Count stayed flat (round 1, codex).
    if (accept_result == ::quietcool::TxAcceptResult::Accepted &&
        request->request.reason == ::quietcool::TxReason::TransactionCommand) {
      pending_tx_command_ =
          PendingTxCommand{request->request.token, request->request.payload.bytes[4]};
    }
    if (accept_result == ::quietcool::TxAcceptResult::Busy &&
        !core_callbacks_.enqueue(::quietcool::CoreCallbackKind::TxRejected,
                                 request->request.token)) {
      degrade("core callback queue capacity exceeded (tx rejected)");
    }
    return;
  }
  if (const auto* revoke = std::get_if<::quietcool::RevokeTxLease>(&effect)) {
    // Clear the latch only when the revocation actually TOOK (round 3, all
    // three engines): revoke_if_unstarted is a no-op once frame 1 is on the
    // air — the burst then runs to completion and genuinely transmits, and
    // clearing here would make TX Count advance while Last TX Command still
    // showed the previous byte. An unstarted revoked lease produces no burst
    // event at all, so this is that case's only clearing site (round 2).
    if (burst_.revoke_if_unstarted(revoke->token) &&
        pending_tx_command_.has_value() &&
        pending_tx_command_->token == revoke->token)
      pending_tx_command_.reset();
    return;
  }
  if (const auto* event = std::get_if<::quietcool::PublishCoreEvent>(&effect)) {
    events_.on_core_event(event->event);
    return;
  }
  if (const auto* persistence =
          std::get_if<::quietcool::RequestPersistenceEffect>(&effect)) {
    // Mirrors core's own sender_ mutations (confirmation_core.cpp: Forget's
    // handle_forget() and Learn's handle_learning_frame() Learned branch),
    // which are always emitted in lockstep with exactly these two
    // PersistenceKind values. Updated unconditionally — regardless of
    // whether the durable write below succeeds — because core's in-RAM
    // sender_ already changed either way; only the durable copy is at risk.
    if (persistence->request.kind ==
        ::quietcool::PersistenceKind::SaveProvisioning) {
      provisioned_sender_ = persistence->request.sender;
      publish_remote_sender_id();
    } else if (persistence->request.kind ==
               ::quietcool::PersistenceKind::EraseProvisioning) {
      provisioned_sender_.reset();
      publish_remote_sender_id();
    }
    if (!preferences_.apply(persistence->request))
      ESP_LOGW(kTag, "Unable to persist requested core state");
    return;
  }
  if (const auto* authority =
          std::get_if<::quietcool::PublishAuthorityEffect>(&effect)) {
    // Fanned out IN PLACE, not only post-drain (rounds 8-9, codex): the core
    // emits this effect in ORDER with its other effects, and a
    // TransactionFinished later in the same batch synchronously publishes
    // "confirmed" — an automation riding that status must see the fan entity
    // AND the *_Known sensors already at their confirmed state, not one batch
    // behind (round 9 caught the sink missing from round 8's fix). The
    // post-drain fan-out remains for the invalidations that emit no effect at
    // all (round 2); double delivery is safe because every consumer dedupes —
    // the sink and fan by revision, the select by shown option, the text
    // diagnostics by last-published string. The sink's clock was set by the
    // apply lambda immediately before this branch runs.
    deliver_authority(authority->authority, events_.now_ms());
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
    // Counts actual transmissions, not commands accepted: BurstComplete only
    // fires once the real 3-frame burst has gone out over the air (never for
    // a request that was refused, deduplicated, or joined to an in-flight
    // transaction — none of those reach BurstTransmitter::accept() again, so
    // none produce a new BurstStarted/BurstComplete pair).
    ++tx_count_;
    // The byte latched at acceptance is genuinely on the air only if THIS
    // completion is the burst that latched it — matched by token, because a
    // latched burst has two ways to die without completing (OEM-priority
    // revocation and a send fault), and an unmatched latch surviving into a
    // later query burst's completion would publish a byte that never
    // transmitted (round 2, all three finding engines).
    //
    // Core is told about the completion FIRST (round 2, codex): publish_state
    // can fire a synchronous automation, and one that issues a command while
    // the core still believes the burst is in flight would be refused or
    // misclassified against stale coordinator state.
    const auto completed_token = complete->token;
    apply_effects(core_.on_tx_complete(completed_token, now_ms), now_ms);
    if (tx_count_sensor_ != nullptr)
      tx_count_sensor_->publish_state(static_cast<float>(tx_count_));
    if (pending_tx_command_.has_value() &&
        pending_tx_command_->token == completed_token) {
      publish_last_tx_command(pending_tx_command_->byte);
      pending_tx_command_.reset();
    }
  } else if (const auto* rejected =
                 std::get_if<::quietcool::BurstRejected>(&event)) {
    if (pending_tx_command_.has_value() &&
        pending_tx_command_->token == rejected->token)
      pending_tx_command_.reset();
    apply_effects(core_.on_tx_rejected(rejected->token, now_ms), now_ms);
  } else if (const auto* fault =
                 std::get_if<::quietcool::BurstFault>(&event)) {
    // A fault after frames already went out is a PARTIAL transmission, and
    // the fan may well have acted on it — the 3-frame burst is redundancy,
    // not a threshold — so the byte that reached the air is still recorded
    // (round 9, codex). But core learns of the fault FIRST, mirroring the
    // complete branch's rule exactly (round 10, opus: publishing first let a
    // synchronous automation command against a core still holding a live
    // in-flight burst). frames_sent is captured before on_tx_fault because
    // the drain it triggers may restart the transmitter.
    std::optional<std::uint8_t> partial_tx_byte;
    if (pending_tx_command_.has_value() &&
        pending_tx_command_->token == fault->token) {
      if (burst_.snapshot().frames_sent > 0)
        partial_tx_byte = pending_tx_command_->byte;
      pending_tx_command_.reset();
    }
    apply_effects(core_.on_tx_fault(fault->token, now_ms), now_ms);
    if (partial_tx_byte) publish_last_tx_command(*partial_tx_byte);
  }
}

// The one function through which every authority snapshot reaches the entity
// publishers and the authority-derived diagnostics — post-drain in production,
// and via publish_authority_for_test in host tests, so the tested wiring IS
// the production wiring. Publishers must tolerate repeated identical
// snapshots: the fan gates on revision, the select dedupes against its shown
// option.
void QuietCoolComponent::deliver_authority(
    const ::quietcool::AuthoritySnapshot& authority,
    ::quietcool::MonotonicMs now_ms) {
  // One delivery, three stages, in failure-direction order (rounds 10-11):
  //  1. PUBLISHERS (fan, select) — the entities whose displayed state other
  //     automations read before acting; they must be current first.
  //  2. SINK (*_Known flags, status) — its automations then see updated
  //     entities. The residual trade of running the sink before stage 3
  //     (round 12, fable — recorded the right way around this time): a
  //     stage-2 sink automation reads stage-3 diagnostic TEXT one delivery
  //     stale. Accepted: the diagnostics are display-only, disabled by
  //     default, and no press composes from them; while a stage-3 automation
  //     reading the Known flags — the direction that fails OPEN on an
  //     invalidation — sees them already updated.
  //  3. DIAGNOSTICS — display only, last.
  // Between 1 and 2: a publisher's synchronous automation may have COMMANDED
  // and changed core authority (round 11, codex). Delivering this snapshot's
  // now-stale Known flags would claim trust the core just withdrew; the
  // reentrant batch has already re-armed publication, so the post-drain
  // delivery — always the latest revision, always last (round 11, opus) —
  // carries the fresh state to the sink instead.
  for (std::size_t index = 0; index < authority_publisher_count_; ++index)
    authority_publishers_[index]->publish_authority(authority);
  // Strictly AHEAD, not merely different: reentrancy can only advance the
  // store's monotonic revision past the snapshot in hand — a delivered
  // revision ahead of the core is unconstructible in production, and != would
  // make the test seam's crafted snapshots indistinguishable from staleness.
  if (core_.authority_revision() > authority.revision) return;
  events_.publish_authority(authority, now_ms);
  // Re-checked between 2 and 3 as well: a SINK automation can command just
  // like a publisher's, and the stale tail it would leave is worse — a
  // diagnostic text (e.g. Last Confirmed Fan State) carrying the pre-command
  // state, whose own automation then composes against a fan the core already
  // knows is changing (round 12, codex). Mid-sink staleness inside stage 2
  // itself is accepted: the remaining sink publications are the Known flags
  // and status text, no press composes from them, and the reentrant batch's
  // post-drain delivery corrects them in the same loop pass.
  if (core_.authority_revision() > authority.revision) return;
  publish_authority_diagnostics(authority);
}

// Change-gated like Last Valid RX Frame and for the same reason (round 10,
// opus): a fault storm re-attempts one byte, and ungated the sensor emitted
// one identical native-API publication — and one automation trigger — per
// faulted attempt.
void QuietCoolComponent::publish_last_tx_command(std::uint8_t byte) {
  if (published_tx_byte_ == byte) return;
  published_tx_byte_ = byte;
  if (last_tx_command_sensor_ != nullptr)
    last_tx_command_sensor_->publish_state(format_hex_byte(byte));
}

void QuietCoolComponent::publish_authority_diagnostics(
    const ::quietcool::AuthoritySnapshot& authority) {
  // Change-gated (round 3, opus, MEASURED): this runs on the post-drain
  // channel, which fires essentially every loop tick — an empty effect batch
  // still arms a publication round — and TextSensor::publish_state notifies
  // the API unconditionally. Ungated, each sensor emitted one state message
  // per tick (~60/s) forever. Every consumer of this channel now carries its
  // own gate: the event sink by revision, the fan by its publication gate,
  // the select by shown-option dedupe, and these two by last-published text.
  // The CHANNEL must stay ungated — its per-tick firing is exactly what
  // delivers the invalidations that emit no effect (round 2, codex).
  if (last_confirmed_state_sensor_ != nullptr) {
    auto text = format_last_confirmed_state(authority.state);
    if (text != published_last_confirmed_state_) {
      published_last_confirmed_state_ = text;
      last_confirmed_state_sensor_->publish_state(std::move(text));
    }
  }
  if (speed_capability_sensor_ != nullptr) {
    auto text = format_speed_capability(authority.speed_capability);
    if (text != published_speed_capability_) {
      published_speed_capability_ = text;
      speed_capability_sensor_->publish_state(std::move(text));
    }
  }
}

void QuietCoolComponent::publish_remote_sender_id() {
  if (remote_sender_id_sensor_ != nullptr)
    remote_sender_id_sensor_->publish_state(format_sender_id(provisioned_sender_));
}

}  // namespace esphome::quietcool
