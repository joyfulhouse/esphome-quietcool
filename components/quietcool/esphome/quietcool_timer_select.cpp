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

  const auto selection = selection_for_option(value);
  if (!selection) {
    // Transmit nothing. Every value this method can send is energizing, so an
    // unrecognised option must not be rounded to the nearest one.
    ESP_LOGW(TAG, "Timer refused: '%s' is not a supported option", value.c_str());
    return;
  }

  // Refresh the cache from the controller's CURRENT snapshot before
  // composing (round 5, codex): within one snapshot delivery the event sink
  // publishes before this entity's cache updates, so a synchronous automation
  // riding e.g. Fan State Known could otherwise command from one-snapshot-
  // stale state. Pulling at press time makes the composition immune to
  // fan-out ordering entirely. Same linked function as the channel path; a
  // publish it deduces is legitimate and deduped identically.
  {
    const auto shown = this->current_option();
    if (const auto option = timer_select_apply_snapshot(
            cache_, controller_->snapshot().authority,
            shown.empty() ? "" : shown.c_str()))
      publish_state(*option);
  }

  // The confirmed-state -> command composition, including the entity/command
  // band pair, lives in timer_command_from_confirmed — a linked, host-tested
  // pure function — precisely so this untestable entity file holds no band
  // decision (round 1, opus: composed inline here, swapping the two bands
  // recreated issue #30 with every suite green).
  const auto command = timer_command_from_confirmed(cache_.confirmed,
                                                    cache_.capability,
                                                    *selection);

  ESP_LOGD(TAG, "Timer '%s' queued as 0x%02X; awaiting confirmation",
           value.c_str(), command.outbound_command_byte());
  controller_->request_state(command);
  // Deliberately no publish_state() here: the shown option changes only when
  // confirmed authority says the fan actually took the timer.
}

}  // namespace esphome::quietcool
