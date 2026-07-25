#include "quietcool/core/response_classifier.h"
#include "support/test.h"

#include <array>
#include <variant>

namespace quietcool {
namespace {

SenderId sender() { return SenderId::from_be_u32(0xCB004739U).value(); }
constexpr std::array<std::uint8_t, 6> kState{0xCB, 0x00, 0x47, 0x39, 0xDF, 0xDF};

QC_TEST("window", "post-command and direct-query boundaries are inclusive") {
  const auto post = ResponseWindow::post_command(TransactionId(1), AttemptNumber(1), 1000);
  QC_CHECK_EQ(post.position_at(1399), WindowPosition::BeforeAcceptance);
  QC_CHECK_EQ(post.position_at(1400), WindowPosition::Accepting);
  QC_CHECK_EQ(post.position_at(2600), WindowPosition::Accepting);
  QC_CHECK_EQ(post.position_at(2601), WindowPosition::ClassificationTail);
  QC_CHECK_EQ(post.position_at(3500), WindowPosition::ClassificationTail);
  QC_CHECK_EQ(post.position_at(3501), WindowPosition::Expired);
  QC_CHECK_EQ(post.position_at(999), WindowPosition::BeforeAcceptance);

  const auto query = ResponseWindow::query(QueryPurpose::Manual, TxToken(4), 2000);
  QC_CHECK_EQ(query.position_at(2299), WindowPosition::BeforeAcceptance);
  QC_CHECK_EQ(query.position_at(2300), WindowPosition::Accepting);
  QC_CHECK_EQ(query.position_at(3100), WindowPosition::Accepting);
  QC_CHECK_EQ(query.position_at(3101), WindowPosition::ClassificationTail);
}

QC_TEST("classification", "exact OEM query has global priority") {
  ResponseClassifier classifier;
  const auto query = FrameCodec::encode_query(sender());
  const auto window = ResponseWindow::post_command(TransactionId(1), AttemptNumber(1), 0);
  const ReceiveContext context = ActiveResponseWindow{window, 7};
  QC_CHECK(std::holds_alternative<ExactOemQuery>(classifier.classify(
      ByteView(query.bytes), sender(), context, 800)));
}

QC_TEST("classification", "post-command early state is ignored not external") {
  ResponseClassifier classifier;
  const auto window = ResponseWindow::post_command(TransactionId(1), AttemptNumber(1), 1000);
  const ReceiveContext context = ActiveResponseWindow{window, 9};
  const auto result = classifier.classify(ByteView(kState), sender(), context, 1399);
  QC_CHECK(std::holds_alternative<IgnoredPostCommandPreAcceptanceState>(result));
}

QC_TEST("classification", "direct-query early state has external priority") {
  ResponseClassifier classifier;
  const auto window = ResponseWindow::query(QueryPurpose::Boot, TxToken(1), 1000);
  const ReceiveContext context = ActiveResponseWindow{window, 3};
  const auto result = classifier.classify(ByteView(kState), sender(), context, 1299);
  QC_CHECK(std::holds_alternative<ExternalPriorityState>(result));
}

QC_TEST("classification", "acceptance candidate retains epoch identity") {
  ResponseClassifier classifier;
  const auto window = ResponseWindow::query(QueryPurpose::Fallback, TxToken(8), 1000);
  const ReceiveContext context = ActiveResponseWindow{window, 42};
  const auto result = classifier.classify(ByteView(kState), sender(), context, 1300);
  QC_CHECK(std::holds_alternative<LocalResponseCandidate>(result));
  QC_CHECK_EQ(std::get<LocalResponseCandidate>(result).epoch_identity, 42U);
}

QC_TEST("classification", "tail repeats and contradictions are quarantined") {
  ResponseClassifier classifier;
  const auto window = ResponseWindow::query(QueryPurpose::Manual, TxToken(3), 1000);
  const ReceiveContext repeat = ClassificationTail{
      window, FanState::observed(0x5F).value(), 11};
  QC_CHECK(std::holds_alternative<LocalTailRepeat>(classifier.classify(
      ByteView(kState), sender(), repeat, 2200)));
  const std::array<std::uint8_t, 6> other{0xCB, 0x00, 0x47, 0x39, 0x2F, 0x2F};
  QC_CHECK(std::holds_alternative<LocalTailContradiction>(classifier.classify(
      ByteView(other), sender(), repeat, 2200)));
}

QC_TEST("classification", "outside an epoch exact state is diagnostic priority") {
  ResponseClassifier classifier;
  QC_CHECK(std::holds_alternative<ExternalPriorityState>(classifier.classify(
      ByteView(kState), sender(), NoLocalEpoch{}, 0)));
  auto wrong = kState;
  wrong[3] = 0x38;
  QC_CHECK(std::holds_alternative<InvalidOrIrrelevant>(classifier.classify(
      ByteView(wrong), sender(), NoLocalEpoch{}, 0)));
}

}  // namespace
}  // namespace quietcool
