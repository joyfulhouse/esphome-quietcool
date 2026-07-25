#include "frame_recovery.h"

namespace quietcool {
namespace {

std::uint8_t bit_count(std::uint8_t value) {
  std::uint8_t count = 0;
  while (value != 0) {
    count = static_cast<std::uint8_t>(count + (value & 1U));
    value >>= 1U;
  }
  return count;
}

}  // namespace

Result<RecoveredResponse, RecoveryError> FrameRecovery::recover_response(
    ByteView input, SenderId provisioned_sender) {
  if (input.size() < 6)
    return Result<RecoveredResponse, RecoveryError>::err(RecoveryError::ShortInput);
  const auto expected = provisioned_sender.bytes();
  if (input[1] != expected[1] || input[2] != expected[2] || input[3] != expected[3])
    return Result<RecoveredResponse, RecoveryError>::err(RecoveryError::SenderMismatch);
  if (input[4] != input[5])
    return Result<RecoveredResponse, RecoveryError>::err(RecoveryError::TailMismatch);
  RecoveryQuality quality = RecoveryQuality::Exact;
  ResponseKind kind = ResponseKind::Normal;
  // The normal-response header is the provisioned sender's own byte[0], derived
  // here the same way decode_strict compares against expected[0] — not the
  // hardcoded 0xCB it used to assume. This keeps the two decoders' notion of
  // sender identity from diverging if a non-0xCB sender ever exists (issue #20).
  // The special-response header 0xCE is a protocol constant, unchanged.
  const auto normal_header = expected[0];
  if (input[0] == 0xCE) {
    kind = ResponseKind::Special;
  } else if (input[0] != normal_header) {
    const auto normal_distance =
        bit_count(static_cast<std::uint8_t>(input[0] ^ normal_header));
    const auto ce_distance = bit_count(static_cast<std::uint8_t>(input[0] ^ 0xCEU));
    if (normal_distance > 1 || normal_distance >= ce_distance)
      return Result<RecoveredResponse, RecoveryError>::err(
          RecoveryError::HeaderNotRecoverable);
    quality = RecoveryQuality::Recovered;
  }
  if (input.size() > 6) quality = RecoveryQuality::Recovered;
  if (input[4] == 0x66)
    return Result<RecoveredResponse, RecoveryError>::err(RecoveryError::Query);
  const auto state = FanState::observed(input[4]);
  if (!state)
    return Result<RecoveredResponse, RecoveryError>::err(RecoveryError::InvalidState);
  return Result<RecoveredResponse, RecoveryError>::ok(
      {quality, kind, state.value()});
}

}  // namespace quietcool

