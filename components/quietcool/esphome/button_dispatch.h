#pragma once

#include <cstdint>

namespace esphome::quietcool {

// The three provisioning actions a controller button can trigger. None of them
// commands the fan — there is deliberately no request_state, speed or duration
// here, so a mislabelled button is a nuisance (a "Refresh" that forgets the
// sender binding), never an unguarded actuation (issue #18).
enum class QuietCoolButtonKind : std::uint8_t { Refresh, Learn, Forget };

// Pure abstraction of the controller methods a button reaches, so the
// kind -> action routing can be unit-tested without the ESPHome Button surface
// (which is why quietcool_button.cpp links into no test target). QuietCoolButton
// backs this with the real controller.
class ButtonActionSink {
 public:
  virtual ~ButtonActionSink() = default;
  virtual void manual_refresh() = 0;
  virtual void learn() = 0;
  virtual void forget() = 0;
};

// Routes a button kind to exactly one provisioning action. This is the whole of
// the dispatch a swapped case label would corrupt.
void dispatch_button_press(QuietCoolButtonKind kind, ButtonActionSink& sink);

}  // namespace esphome::quietcool
