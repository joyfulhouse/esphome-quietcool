#include "quietcool_timer_select.h"

#include "esphome/core/log.h"

namespace esphome::quietcool {

namespace {
constexpr char TAG[] = "quietcool.timer";
}  // namespace

void QuietCoolTimerSelect::dump_config() {
  ESP_LOGCONFIG(TAG, "QuietCool Fan Timer");
  ESP_LOGCONFIG(TAG, "  Every option transmits: a duration starts the fan (at "
                     "LOW if stopped); None means run continuously and will "
                     "restart a fan whose timer expired.");
  if (controller_ == nullptr)
    ESP_LOGE(TAG, "  Timer select refused: controller is not configured");
}

void QuietCoolTimerSelect::publish_authority(
    const ::quietcool::AuthoritySnapshot& authority) {
  // The ENTIRE per-snapshot update — capability caching, the reason-
  // discriminating confirmed-state rule, and the shown-option dedupe — is one
  // linked, tested call (round 3, opus: mutation proved anything left in this
  // file is unreachable by any test; this line is now all there is to leave).
  const auto shown = this->current_option();
  if (const auto option = timer_select_apply_snapshot(
          cache_, authority, shown.empty() ? "" : shown.c_str()))
    publish_state(*option);
}

void QuietCoolTimerSelect::control(const std::string& value) {
  if (controller_ == nullptr) {
    ESP_LOGE(TAG, "Timer refused: controller is not configured");
    return;
  }
  if (controller_->degraded()) {
    // The degradation contract says control and observation are OVER (issue
    // #9); request_state would silently no-op, so refuse loudly instead of
    // logging "queued" for a command that can never transmit (round 6).
    ESP_LOGE(TAG, "Timer refused: controller is degraded");
    return;
  }

  const auto selection = selection_for_option(value);
  if (!selection) {
    // Transmit nothing. Every value this method can send is energizing, so an
    // unrecognised option must not be rounded to the nearest one.
    ESP_LOGW(TAG, "Timer refused: '%s' is not a supported option", value.c_str());
    return;
  }

  // Refresh the cache from the controller's CURRENT snapshot before
  // composing (rounds 5-11): this entity's channel-fed cache can lag the core
  // by one delivery — publisher-array ORDER decides who hears a snapshot
  // first, and a synchronous automation can command between deliveries — so
  // the press always composes from the live core state, immune to fan-out
  // ordering entirely. CACHE ONLY — deliberately no publish_state here
  // (round 6, codex): publishing inside control() let the OUTER press
  // supersede a synchronous on_value automation's nested command; the shown
  // option keeps moving exclusively on the authority fan-out channel. The
  // discarded return value is that unpublished option.
  const auto authority = controller_->snapshot().authority;
  (void)timer_select_apply_snapshot(cache_, authority, "");

  // The whole press composition — the entity/command band pair and the
  // due-deadline rule — is one linked, host-tested pure function; this
  // untestable entity file holds no band decision (rounds 1-6).
  const auto command = timer_command_for_press(cache_, authority,
                                               controller_->now_ms(),
                                               *selection);

  ESP_LOGD(TAG, "Timer '%s' queued as 0x%02X; awaiting confirmation",
           value.c_str(), command.outbound_command_byte());
  controller_->request_state(command);
  // Deliberately no publish_state() here: the shown option changes only when
  // confirmed authority says the fan actually took the timer.
}

}  // namespace esphome::quietcool
