// Button kind -> provisioning-action dispatch (issue #18).
//
// quietcool_button.cpp is linked into no test binary (it derives from
// esphome::button::Button). A swapped case label there is not an actuation
// hazard — buttons never reach request_state, only request_manual_refresh /
// request_learn / request_forget — but it would misroute a provisioning action,
// e.g. a "Refresh" button that instead forgets the sender binding and forces a
// re-learn. The routing is now the pure dispatch_button_press(); this pins it.

#include "quietcool/esphome/button_dispatch.h"

#include "support/test.h"

#include <string>

namespace esphome::quietcool {
namespace {

// Records the single action a dispatch produced.
class RecordingSink final : public ButtonActionSink {
 public:
  void manual_refresh() override { record("manual_refresh"); }
  void learn() override { record("learn"); }
  void forget() override { record("forget"); }

  std::string last;
  int count{0};

 private:
  void record(const char* action) {
    last = action;
    ++count;
  }
};

// Each kind must reach exactly one action, and the intended one. Mutation: swap
// any case label in dispatch_button_press (e.g. Refresh -> forget) — the routed
// action no longer matches. A dropped case would leave count at 0.
QC_TEST("button_dispatch", "each kind routes to its one provisioning action") {
  struct Case final {
    QuietCoolButtonKind kind;
    const char* action;
  };
  const Case cases[] = {
      {QuietCoolButtonKind::Refresh, "manual_refresh"},
      {QuietCoolButtonKind::Learn, "learn"},
      {QuietCoolButtonKind::Forget, "forget"},
  };
  for (const auto& c : cases) {
    RecordingSink sink;
    dispatch_button_press(c.kind, sink);
    QC_CHECK_EQ(sink.count, 1);
    QC_CHECK_EQ(sink.last, std::string(c.action));
  }
}

}  // namespace
}  // namespace esphome::quietcool
