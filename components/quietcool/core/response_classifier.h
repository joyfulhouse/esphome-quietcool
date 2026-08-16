#pragma once

#include "frame_recovery.h"
#include "response_window.h"

namespace quietcool {

struct NoLocalEpoch final {};
struct PassiveObservationContext final {};
struct ActiveResponseWindow final {
  ResponseWindow window;
  std::uint32_t epoch_identity;
  std::optional<FanState> tracked_state{};
};
struct ClassificationTail final {
  ResponseWindow window;
  FanState expected_state;
  std::uint32_t epoch_identity;
};
using ReceiveContext = std::variant<NoLocalEpoch, PassiveObservationContext,
                                    ActiveResponseWindow, ClassificationTail>;

struct ExactOemQuery final {};
struct LocalResponseCandidate final {
  RecoveredResponse response;
  std::uint32_t epoch_identity;
};
struct LocalTailRepeat final { FanState state; std::uint32_t epoch_identity; };
struct LocalTailContradiction final { FanState state; std::uint32_t epoch_identity; };
struct ExternalPriorityState final { FanState state; };
struct PassiveResponseOnlyCandidate final { RecoveredResponse response; };
struct PassiveAmbiguousCandidate final { RecoveredResponse response; };
struct IgnoredPostCommandPreAcceptanceState final { FanState state; };
struct SpecialDiagnostic final { FanState state; };
struct InvalidOrIrrelevant final {};
using ClassifiedFrame = std::variant<ExactOemQuery, LocalResponseCandidate,
    LocalTailRepeat, LocalTailContradiction, ExternalPriorityState,
    PassiveResponseOnlyCandidate, PassiveAmbiguousCandidate,
    IgnoredPostCommandPreAcceptanceState, SpecialDiagnostic, InvalidOrIrrelevant>;

class ResponseClassifier final {
 public:
  ClassifiedFrame classify(ByteView input, SenderId sender,
                           const ReceiveContext& context,
                           MonotonicMs now_ms) const;
};

}  // namespace quietcool
