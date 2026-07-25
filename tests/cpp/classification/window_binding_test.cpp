#include "quietcool/core/response_classifier.h"
#include "support/test.h"

#include <array>
#include <variant>

// H3 regression guard (see SECURITY.md, "Window binding"): a sender-matching,
// recoverable state frame is only ever promoted to a confirmation candidate
// while the bridge is listening inside a window it opened itself. Off-window
// traffic — including a frame arriving when no local window exists at all —
// must never become a LocalResponseCandidate, so it can never contribute to a
// "confirmed" state. This documents that acceptance is bound to our window, not
// to the mere presence of a well-formed frame on air.

namespace quietcool {
namespace {

SenderId sender() { return SenderId::from_be_u32(0xCB004739U).value(); }
constexpr std::array<std::uint8_t, 6> kState{0xCB, 0x00, 0x47, 0x39, 0xDF, 0xDF};

QC_TEST("classification", "off-window frames never become response candidates") {
  ResponseClassifier classifier;

  // No local window open at all: the same frame that would confirm in-window
  // is not even a candidate.
  const ReceiveContext no_window = NoLocalEpoch{};
  QC_CHECK(!std::holds_alternative<LocalResponseCandidate>(
      classifier.classify(ByteView(kState), sender(), no_window, 1500)));

  // A window exists but the frame lands after it has expired (outside the
  // accepting region). Still not a candidate. This is the assertion the
  // window-position gate (response_classifier.cpp:53) is responsible for; the
  // mutation below removes that gate to prove the guard has teeth.
  const auto window =
      ResponseWindow::post_command(TransactionId(1), AttemptNumber(1), 1000);
  QC_CHECK_EQ(window.position_at(4000), WindowPosition::Expired);
  const ReceiveContext expired = ActiveResponseWindow{window, 7};
  QC_CHECK(!std::holds_alternative<LocalResponseCandidate>(
      classifier.classify(ByteView(kState), sender(), expired, 4000)));

  // Positive control: the identical frame inside the accepting window IS a
  // candidate, so the negative assertions above are about the window, not a
  // frame the classifier could never accept.
  const ReceiveContext accepting = ActiveResponseWindow{window, 7};
  QC_CHECK(std::holds_alternative<LocalResponseCandidate>(
      classifier.classify(ByteView(kState), sender(), accepting, 1500)));
}

}  // namespace
}  // namespace quietcool
