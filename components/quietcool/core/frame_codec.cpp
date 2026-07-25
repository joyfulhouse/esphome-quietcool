#include "frame_codec.h"

namespace quietcool {

FrameBytes FrameCodec::encode_query(SenderId sender) {
  const auto id = sender.bytes();
  return {{id[0], id[1], id[2], id[3], 0x66, 0x66}};
}

Result<FrameBytes, FrameEncodeError> FrameCodec::encode_state(SenderId sender,
                                                               FanState state) {
  if (!state.speed())
    return Result<FrameBytes, FrameEncodeError>::err(FrameEncodeError::MissingRememberedSpeed);
  if (!duration_from_value(state.raw_byte() & 0x0FU))
    return Result<FrameBytes, FrameEncodeError>::err(FrameEncodeError::InvalidCommandDuration);
  const auto id = sender.bytes();
  const auto byte = state.outbound_command_byte();
  return Result<FrameBytes, FrameEncodeError>::ok(
      {{id[0], id[1], id[2], id[3], byte, byte}});
}

Result<DecodedFrame, FrameDecodeError> FrameCodec::decode_strict(
    ByteView input, SenderId provisioned_sender) {
  if (input.size() != 6)
    return Result<DecodedFrame, FrameDecodeError>::err(FrameDecodeError::InvalidLength);
  if (input[4] != input[5])
    return Result<DecodedFrame, FrameDecodeError>::err(FrameDecodeError::TailMismatch);
  const auto expected = provisioned_sender.bytes();
  const bool normal = input[0] == expected[0] && input[1] == expected[1] &&
                      input[2] == expected[2] && input[3] == expected[3];
  const bool special = input[0] == 0xCE && input[1] == expected[1] &&
                       input[2] == expected[2] && input[3] == expected[3];
  if (!normal && !special)
    return Result<DecodedFrame, FrameDecodeError>::err(FrameDecodeError::SenderMismatch);
  if (input[4] == 0x66) {
    if (special)
      return Result<DecodedFrame, FrameDecodeError>::err(FrameDecodeError::SpecialQuery);
    return Result<DecodedFrame, FrameDecodeError>::ok(ExactQuery{provisioned_sender});
  }
  const auto state = FanState::observed(input[4]);
  if (!state)
    return Result<DecodedFrame, FrameDecodeError>::err(FrameDecodeError::InvalidState);
  if (special)
    return Result<DecodedFrame, FrameDecodeError>::ok(
        ExactSpecialResponse{provisioned_sender, state.value()});
  return Result<DecodedFrame, FrameDecodeError>::ok(
      ExactState{provisioned_sender, state.value()});
}

}  // namespace quietcool
