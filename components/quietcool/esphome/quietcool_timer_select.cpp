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

  // The speed nibble a timer command carries comes only from CONFIRMED state,
  // through the same gate the fan entity uses.
  if (const auto confirmed = publication_gate_.next(authority))
    confirmed_ = confirmed->state;

  // The published option comes from the timer authority itself, which the
  // authority store only ever writes from confirmed evidence. nullopt means
  // "not known" — leave the shown option alone rather than assert a timer state
  // nothing supports.
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

  // Speed round-trips through the ENTITY band and back through the COMMAND
  // band, and that asymmetry is deliberate. While capability is unknown the
  // command band is 2, which structurally cannot form MED — so a confirmed MED
  // clamps to HIGH rather than reaching a fan that may not have MED at all
  // (issues #30, #31). Do not "simplify" this to one band.
  const auto feedback =
      confirmed_ ? authority_to_feedback(*confirmed_, entity_speed_count(capability_))
                 : FanFeedback{};
  const auto command = timer_command_from_intent(
      *selection, feedback.on, feedback.speed.value_or(1),
      command_speed_count(capability_));

  ESP_LOGD(TAG, "Timer '%s' queued as 0x%02X; awaiting confirmation",
           value.c_str(), command.outbound_command_byte());
  controller_->request_state(command);
  // Deliberately no publish_state() here: the shown option changes only when
  // confirmed authority says the fan actually took the timer.
}

}  // namespace esphome::quietcool
