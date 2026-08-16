#include "quietcool/core/response_classifier.h"
#include "support/test.h"

#include <array>
#include <variant>

namespace quietcool {
namespace {

SenderId passive_classifier_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

template <std::uint8_t Value>
ClassifiedFrame classify_idle_value() {
  constexpr std::array<std::uint8_t, 6> frame{
      0xCB, 0x00, 0x47, 0x39, Value, Value};
  return ResponseClassifier{}.classify(
      ByteView(frame), passive_classifier_sender(), PassiveObservationContext{},
      1000);
}

QC_TEST("passive_classifier",
        "captured response-only encoding is typed as decisive") {
  // Preserved capture/prose evidence: 1F 1F is fan-attributed and bit 7 clear,
  // so it cannot be emitted by the documented ordinary remote constructor.
  QC_CHECK(std::holds_alternative<PassiveResponseOnlyCandidate>(
      classify_idle_value<0x1F>()));

  // Synthetic protocol-model boundary vectors, not claimed captures.
  constexpr std::array<std::uint8_t, 6> malformed{
      0xCB, 0x00, 0x47, 0x39, 0x1F, 0x9F};
  constexpr std::array<std::uint8_t, 6> foreign{
      0xCB, 0x00, 0x47, 0x38, 0x1F, 0x1F};
  const ResponseClassifier classifier;
  const PassiveObservationContext idle{};
  QC_CHECK(std::holds_alternative<InvalidOrIrrelevant>(classifier.classify(
      ByteView(malformed), passive_classifier_sender(), idle, 1000)));
  QC_CHECK(std::holds_alternative<InvalidOrIrrelevant>(classifier.classify(
      ByteView(foreign), passive_classifier_sender(), idle, 1000)));
}

QC_TEST("passive_classifier",
        "captured command-shaped encoding remains ambiguous") {
  // Preserved evidence boundary: BF is both an ordinary High/Continuous
  // command encoding and a fan-attributed value. Bytes alone cannot name its
  // sender, so timing/burst order is still required.
  QC_CHECK(std::holds_alternative<PassiveAmbiguousCandidate>(
      classify_idle_value<0xBF>()));
}

QC_TEST("passive_classifier",
        "all marker classes retain the conservative evidence boundary") {
  // Synthetic marker-class boundaries, not claimed captures. The settled
  // evidence contract makes bit 7 clear decisive and keeps every bit-7-set
  // value ambiguous, including capability marker class 11.
  QC_CHECK(std::holds_alternative<PassiveResponseOnlyCandidate>(
      classify_idle_value<0x1F>()));  // 00
  QC_CHECK(std::holds_alternative<PassiveResponseOnlyCandidate>(
      classify_idle_value<0x5F>()));  // 01
  QC_CHECK(std::holds_alternative<PassiveAmbiguousCandidate>(
      classify_idle_value<0x9F>()));  // 10
  QC_CHECK(std::holds_alternative<PassiveAmbiguousCandidate>(
      classify_idle_value<0xDF>()));  // 11
}

}  // namespace
}  // namespace quietcool
