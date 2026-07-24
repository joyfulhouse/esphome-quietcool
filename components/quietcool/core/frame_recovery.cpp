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
  if (input[0] == 0xCE) {
    kind = ResponseKind::Special;
  } else if (input[0] != 0xCB) {
    const auto cb_distance = bit_count(static_cast<std::uint8_t>(input[0] ^ 0xCBU));
    const auto ce_distance = bit_count(static_cast<std::uint8_t>(input[0] ^ 0xCEU));
    if (cb_distance > 1 || cb_distance >= ce_distance)
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

