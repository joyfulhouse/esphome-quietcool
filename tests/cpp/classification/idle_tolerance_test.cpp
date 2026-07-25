// Error tolerance must not depend on whether the bridge happens to be
// listening for its own reply.
//
// FrameRecovery::recover_response accepts >= 6 bytes and corrects a single-bit
// error in byte[0], but before this fix the classifier only reached it inside
// an accepting response window — i.e. for frames answering a command the bridge
// just sent, when it already knew what it expected. Frames that originate
// autonomously (the OEM remote, and the fan's unsolicited ~1203 ms self-report)
// arrive with no local window open, or land in a window's before-acceptance or
// classification-tail position, and were decoded strictly: exactly 6 bytes,
// byte[0] an exact match. A corrupted one was then dropped with no event and no
// log, because on_frame bare-returns on InvalidOrIrrelevant.
//
// Field consequence: press the OEM remote, the fan runs, every report it emits
// is discarded, and Home Assistant keeps displaying the previous confirmed
// state indefinitely. That is a wrong state presented as fact, invisible in the
// logs.
//
// The corruption is real, not hypothetical. A live capture from the deployed
// controller recorded a truncated 3-byte frame, and the predecessor YAML build
// documented that a corrupted 0x06 length byte makes the SX1278 deliver a
// different length.
//
// These tests pin the property "an idle-or-windowed bridge tolerates the same
// corruption an in-acceptance bridge does" while refusing exactly the frames
// the recovery layer refuses by design (0xCA / 0xCF, which are Hamming-1 from
// both the normal 0xCB and special 0xCE headers), foreign senders, and
// mismatched tails.

#include "quietcool/core/response_classifier.h"
#include "support/test.h"

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

namespace quietcool {
namespace {

SenderId sender() { return SenderId::from_be_u32(0xCB004739U).value(); }

// An OEM "fan ON, high, continuous" report: marker bit set, repeated tail.
constexpr std::uint8_t kCmd = 0xBF;

ByteView view(const std::vector<std::uint8_t>& bytes) {
  return ByteView(bytes.data(), bytes.size());
}

// No local epoch: the coordinator is idle, its normal resting state and
// precisely when the OEM remote is used.
ReceiveContext idle() { return NoLocalEpoch{}; }

// A before-acceptance position of a direct-query window: the fan's ~1203 ms
// self-report lands here (window opens at 1000, accepts from 1300).
ReceiveContext before_acceptance() {
  return ActiveResponseWindow{
      ResponseWindow::query(QueryPurpose::Boot, TxToken(1), 1000), 3};
}

// A classification-tail position of a post-command window (opened at 1000,
// accepts 1400..2600, tail 2601..3500): a late OEM report lands here.
ReceiveContext post_command_tail() {
  return ActiveResponseWindow{
      ResponseWindow::post_command(TransactionId(1), AttemptNumber(1), 1000), 5};
}

QC_TEST("idle_tolerance", "clean external report is heard while idle") {
  // Control case: proves the fixture is sound, so a failure below is about
  // corruption tolerance and not about the setup.
  const std::vector<std::uint8_t> clean{0xCB, 0x00, 0x47, 0x39, kCmd, kCmd};
  const ResponseClassifier classifier;
  const auto out = classifier.classify(view(clean), sender(), idle(), 10'000);
  QC_CHECK(std::holds_alternative<ExternalPriorityState>(out));
}

QC_TEST("idle_tolerance", "single-bit header error is heard while idle") {
  // 0xCB -> 0x4B: the bit-7 (marker) flip. recover_response corrects exactly
  // this (cb_distance 1, ce_distance 3), but before the fix only inside an
  // accepting window; decode_strict rejects it as a SenderMismatch.
  const std::vector<std::uint8_t> flipped{0x4B, 0x00, 0x47, 0x39, kCmd, kCmd};
  const ResponseClassifier classifier;
  const auto out = classifier.classify(view(flipped), sender(), idle(), 10'000);
  QC_CHECK(std::holds_alternative<ExternalPriorityState>(out));
}

QC_TEST("idle_tolerance", "ambiguous header 0xCA stays invalid while idle") {
  // 0xCA is Hamming-1 from BOTH 0xCB (normal) and 0xCE (special), so correcting
  // it would be a coin flip between a state report and a diagnostic.
  // recover_response refuses it by design (cb_distance 1 >= ce_distance 1), and
  // widening byte[0] tolerance must never cross that line. This is the guard
  // that stops the fix from drifting into guessing.
  const std::vector<std::uint8_t> ambiguous{0xCA, 0x00, 0x47, 0x39, kCmd, kCmd};
  const ResponseClassifier classifier;
  const auto out = classifier.classify(view(ambiguous), sender(), idle(), 10'000);
  QC_CHECK(std::holds_alternative<InvalidOrIrrelevant>(out));
}

QC_TEST("idle_tolerance", "overlength report with intact prefix is heard while idle") {
  // A corrupted length byte makes the radio hand up trailing bytes after an
  // otherwise perfect frame. recover_response ignores the tail; decode_strict
  // rejects the whole buffer on length alone.
  const std::vector<std::uint8_t> overlong{0xCB, 0x00, 0x47, 0x39, kCmd, kCmd,
                                           0xAA, 0x55};
  const ResponseClassifier classifier;
  const auto out = classifier.classify(view(overlong), sender(), idle(), 10'000);
  QC_CHECK(std::holds_alternative<ExternalPriorityState>(out));
}

QC_TEST("idle_tolerance", "overlength report is heard before acceptance too") {
  // The fan's unsolicited ~1203 ms self-report lands in a window's
  // before-acceptance region. Tolerance must reach this branch, not only the
  // idle one and the accepting one.
  const std::vector<std::uint8_t> overlong{0xCB, 0x00, 0x47, 0x39, kCmd, kCmd,
                                           0xAA, 0x55};
  const ResponseClassifier classifier;
  const auto out =
      classifier.classify(view(overlong), sender(), before_acceptance(), 1203);
  QC_CHECK(std::holds_alternative<ExternalPriorityState>(out));
}

QC_TEST("idle_tolerance", "overlength report is classified in the tail too") {
  // A late report landing in the classification-tail position must still be
  // seen (as a tail repeat/contradiction), not silently dropped on length.
  const std::vector<std::uint8_t> overlong{0xCB, 0x00, 0x47, 0x39, kCmd, kCmd,
                                           0xAA, 0x55};
  const ResponseClassifier classifier;
  const auto out =
      classifier.classify(view(overlong), sender(), post_command_tail(), 2700);
  QC_CHECK(std::holds_alternative<LocalTailRepeat>(out) ||
           std::holds_alternative<LocalTailContradiction>(out));
  QC_CHECK(!std::holds_alternative<InvalidOrIrrelevant>(out));
}

QC_TEST("idle_tolerance", "overlength normal query is heard as an OEM query") {
  // A corrupted length byte on an OEM query (0x66 tail, normal 0xCB header) is
  // heard as OEM activity exactly as an exact query is; decode_strict rejects
  // it on length. This is the query half of the branch-independent tolerance.
  const std::vector<std::uint8_t> query{0xCB, 0x00, 0x47, 0x39, 0x66, 0x66,
                                        0xAA};
  const ResponseClassifier classifier;
  const auto out = classifier.classify(view(query), sender(), idle(), 10'000);
  QC_CHECK(std::holds_alternative<ExactOemQuery>(out));
}

QC_TEST("idle_tolerance", "exact special query stays unheard while idle") {
  // A 0xCE + 0x66 frame is decode_strict's SpecialQuery, deliberately distinct
  // from ExactQuery. 0xCE semantics are uncharacterised and must never assert
  // OEM priority (which would invalidate authority), so it stays unheard —
  // exactly as before this change. This guards the query half against widening
  // into an ununderstood header.
  const std::vector<std::uint8_t> special_query{0xCE, 0x00, 0x47, 0x39, 0x66,
                                                0x66};
  const ResponseClassifier classifier;
  const auto out =
      classifier.classify(view(special_query), sender(), idle(), 10'000);
  QC_CHECK(std::holds_alternative<InvalidOrIrrelevant>(out));
  QC_CHECK(!std::holds_alternative<ExactOemQuery>(out));
}

QC_TEST("idle_tolerance", "overlength special query stays unheard while idle") {
  // The overlength form must not slip through the recovery path either.
  const std::vector<std::uint8_t> special_query{0xCE, 0x00, 0x47, 0x39, 0x66,
                                                0x66, 0xAA};
  const ResponseClassifier classifier;
  const auto out =
      classifier.classify(view(special_query), sender(), idle(), 10'000);
  QC_CHECK(std::holds_alternative<InvalidOrIrrelevant>(out));
  QC_CHECK(!std::holds_alternative<ExactOemQuery>(out));
}

QC_TEST("idle_tolerance", "foreign sender is still rejected while idle") {
  // The tolerance must widen only for corruption of OUR sender, never into
  // accepting another unit. The upstairs fan (0xCB03D7D3) is genuinely on air
  // on the same frequency with identical framing, so this is a live case, and
  // it differs from ours in bytes [1..3] rather than a single bit in byte[0].
  const std::vector<std::uint8_t> upstairs{0xCB, 0x03, 0xD7, 0xD3, kCmd, kCmd};
  const ResponseClassifier classifier;
  const auto out = classifier.classify(view(upstairs), sender(), idle(), 10'000);
  QC_CHECK(std::holds_alternative<InvalidOrIrrelevant>(out));
}

QC_TEST("idle_tolerance", "mismatched tail is still rejected while idle") {
  // byte[4] != byte[5] means the command byte itself is unreliable; both
  // decoders reject it and widening tolerance must not change that.
  const std::vector<std::uint8_t> bad_tail{0xCB, 0x00, 0x47, 0x39, kCmd, 0x00};
  const ResponseClassifier classifier;
  const auto out = classifier.classify(view(bad_tail), sender(), idle(), 10'000);
  QC_CHECK(std::holds_alternative<InvalidOrIrrelevant>(out));
}

}  // namespace
}  // namespace quietcool
