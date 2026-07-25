#pragma once

#include "fan_state.h"
#include "sender_id.h"

namespace quietcool {

enum class FrameEncodeError : std::uint8_t { MissingRememberedSpeed, InvalidCommandDuration };
enum class FrameDecodeError : std::uint8_t {
  InvalidLength, TailMismatch, SenderMismatch, SpecialQuery, InvalidState
};

struct ExactQuery final { SenderId sender; };
struct ExactState final { SenderId sender; FanState state; };
struct ExactSpecialResponse final { SenderId provisioned_sender; FanState state; };
using DecodedFrame = std::variant<ExactQuery, ExactState, ExactSpecialResponse>;

class FrameCodec final {
 public:
  static FrameBytes encode_query(SenderId sender);
  static Result<FrameBytes, FrameEncodeError> encode_state(SenderId sender,
                                                            FanState state);
  static Result<DecodedFrame, FrameDecodeError> decode_strict(
      ByteView input, SenderId provisioned_sender);
};

}  // namespace quietcool

