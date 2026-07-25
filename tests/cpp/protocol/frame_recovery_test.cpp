// Recovery-decoder tests. The point of issue #20 is that FrameRecovery and
// FrameCodec must agree on *sender identity*: the recovery path used to hardcode
// the 0xCB normal header while decode_strict derived it from the provisioned
// sender, so the two only agreed because every provisionable sender begins with
// 0xCB. That prefix is in fact enforced by SenderId itself (sender_id.cpp), so a
// non-0xCB sender cannot be constructed here; these tests therefore pin the
// observable recovery contract (the ambiguity tie-break and Hamming-1 recovery)
// and the exact-identity agreement, and the alignment's non-0xCB effect is
// demonstrated separately in the review notes.

#include "quietcool/core/frame_recovery.h"

#include "quietcool/core/frame_codec.h"

#include "support/test.h"

#include <array>
#include <cstdint>

namespace quietcool {
namespace {

SenderId sender() { return SenderId::from_be_u32(0xCB004739U).value(); }

// A well-formed six-byte report: 4-byte header/sender + a repeated state byte.
std::array<std::uint8_t, 6> frame(std::uint8_t header, std::uint8_t b1,
                                  std::uint8_t b2, std::uint8_t b3,
                                  std::uint8_t state) {
  return {header, b1, b2, b3, state, state};
}

constexpr std::uint8_t kState = 0x1F;  // observed Low/Continuous — a valid report.

// #20: on an exact frame the strict and recovery decoders must agree on sender
// identity — the property that held only textually before, because both files
// independently assumed the 0xCB prefix.
QC_TEST("protocol", "strict and recovery decoders agree on exact identity") {
  // A genuine frame from our own fan: both accept.
  const auto ours = frame(0xCB, 0x00, 0x47, 0x39, kState);
  QC_CHECK(FrameCodec::decode_strict(ByteView(ours), sender()).has_value());
  QC_CHECK(FrameRecovery::recover_response(ByteView(ours), sender()).has_value());

  // Same header byte, different sender body: both reject on identity, not tolerance.
  const auto foreign = frame(0xCB, 0x00, 0x47, 0x3A, kState);
  QC_CHECK(!FrameCodec::decode_strict(ByteView(foreign), sender()).has_value());
  QC_CHECK(!FrameRecovery::recover_response(ByteView(foreign), sender()).has_value());

  // Special-response header 0xCE: both accept, and recovery flags it Special.
  const auto special = frame(0xCE, 0x00, 0x47, 0x39, kState);
  QC_CHECK(FrameCodec::decode_strict(ByteView(special), sender()).has_value());
  const auto recovered_special =
      FrameRecovery::recover_response(ByteView(special), sender());
  QC_CHECK(recovered_special.has_value());
  QC_CHECK(recovered_special.value().kind == ResponseKind::Special);
}

// The Hamming-1 tie-break that rejects 0xCA — ambiguous between 0xCB (normal)
// and 0xCE (special) — must be preserved exactly. Mutation: relax the tie-break
// (`>=` to `>`) and 0xCA is wrongly recovered, failing this test.
QC_TEST("protocol", "ambiguous 0xCA header stays rejected") {
  const auto ambiguous = frame(0xCA, 0x00, 0x47, 0x39, kState);
  QC_CHECK(!FrameRecovery::recover_response(ByteView(ambiguous), sender())
                .has_value());
}

// A single-bit header corruption unambiguously near the normal header recovers,
// and is reported Recovered (not Exact) so consensus keeps the 3-vs-2 distinction.
// Mutation: tighten `normal_distance > 1` to `> 0` and this legitimate recovery
// is rejected. decode_strict, being strict, does not tolerate the flip.
QC_TEST("protocol", "single-bit header corruption recovers as Recovered") {
  const auto corrupt = frame(0xC9, 0x00, 0x47, 0x39, kState);  // 0xCB ^ 0x02
  const auto result = FrameRecovery::recover_response(ByteView(corrupt), sender());
  QC_CHECK(result.has_value());
  QC_CHECK(result.value().quality == RecoveryQuality::Recovered);
  QC_CHECK(!FrameCodec::decode_strict(ByteView(corrupt), sender()).has_value());
}

}  // namespace
}  // namespace quietcool
