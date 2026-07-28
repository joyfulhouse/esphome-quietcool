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
  // Cache the capability unconditionally: like the fan's band, it is sticky and
  // survives an authority invalidation, because capability describes the bound
  // fan rather than the freshness of what we know about it (issue #31).
  capability_ = authority.speed_capability;

  // confirmed_ mirrors the snapshot's CURRENT state authority, every
  // publication — confirmed in, cleared out. It must NOT latch through a
  // revision gate the way the fan's display value does: an invalidation
  // (timer expiry, Forget/Learn re-binding, an in-flight command) must empty
  // this cache, or the next timer command is aimed with a state the fan no
  // longer has. Round 1 found both energizing shapes of that staleness: a fan
  // whose timer expired re-started as running-HIGH instead of stopped-LOW
  // (codex, high), and a freshly-bound fan started at the PREVIOUS fan's
  // speed (opus). Not-confirmed maps to nullopt, which
  // timer_command_from_confirmed treats as the documented stopped-fan rule.
  if (const auto* confirmed =
          std::get_if<::quietcool::ConfirmedStateAuthority>(&authority.state))
    confirmed_ = confirmed->state;
  else
    confirmed_ = std::nullopt;

  // The published option comes from the timer authority itself, which the
  // authority store only ever writes from confirmed evidence. nullopt means
  // "not known" — leave the shown option alone rather than assert a timer state
  // nothing supports. Note the DISPLAY sense of "None" is "no timer
  // programmed", which is true for a stopped fan too; only the COMMAND sense
  // of selecting it transmits run-continuously.
  if (const auto option = timer_option_for_authority(authority))
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

  // The confirmed-state -> command composition, including the entity/command
  // band pair, lives in timer_command_from_confirmed — a linked, host-tested
  // pure function — precisely so this untestable entity file holds no band
  // decision (round 1, opus: composed inline here, swapping the two bands
  // recreated issue #30 with every suite green).
  const auto command =
      timer_command_from_confirmed(confirmed_, capability_, *selection);

  ESP_LOGD(TAG, "Timer '%s' queued as 0x%02X; awaiting confirmation",
           value.c_str(), command.outbound_command_byte());
  controller_->request_state(command);
  // Deliberately no publish_state() here: the shown option changes only when
  // confirmed authority says the fan actually took the timer.
}

}  // namespace esphome::quietcool
