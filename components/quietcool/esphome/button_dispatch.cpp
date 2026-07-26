#include "button_dispatch.h"

namespace esphome::quietcool {

void dispatch_button_press(QuietCoolButtonKind kind, ButtonActionSink& sink) {
  switch (kind) {
    case QuietCoolButtonKind::Refresh:
      sink.manual_refresh();
      return;
    case QuietCoolButtonKind::Learn:
      sink.learn();
      return;
    case QuietCoolButtonKind::Forget:
      sink.forget();
      return;
  }
}

}  // namespace esphome::quietcool
