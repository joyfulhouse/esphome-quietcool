#pragma once

#include "frame_codec.h"

namespace quietcool {

enum class RecoveryQuality : std::uint8_t { Exact, Recovered };
enum class ResponseKind : std::uint8_t { Normal, Special };
enum class RecoveryError : std::uint8_t {
  ShortInput, SenderMismatch, HeaderNotRecoverable, TailMismatch, Query,
  InvalidState
};

struct RecoveredResponse final {
  RecoveryQuality quality;
  ResponseKind kind;
  FanState state;
};

class FrameRecovery final {
 public:
  static Result<RecoveredResponse, RecoveryError> recover_response(
      ByteView input, SenderId provisioned_sender);
};

}  // namespace quietcool
