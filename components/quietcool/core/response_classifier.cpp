#include "response_classifier.h"

namespace quietcool {
namespace {

std::optional<FanState> strict_state(const DecodedFrame& decoded,
                                     bool* special = nullptr) {
  if (const auto* state = std::get_if<ExactState>(&decoded)) {
    if (special) *special = false;
    return state->state;
  }
  if (const auto* response = std::get_if<ExactSpecialResponse>(&decoded)) {
    if (special) *special = true;
    return response->state;
  }
  return std::nullopt;
}

}  // namespace

ClassifiedFrame ResponseClassifier::classify(ByteView input, SenderId sender,
                                             const ReceiveContext& context,
                                             MonotonicMs now_ms) const {
  const auto strict = FrameCodec::decode_strict(input, sender);
  if (strict && std::holds_alternative<ExactQuery>(strict.value()))
    return ExactOemQuery{};
  bool special = false;
  const auto state = strict ? strict_state(strict.value(), &special)
                            : std::nullopt;
  if (special && state) return SpecialDiagnostic{*state};
  if (std::holds_alternative<NoLocalEpoch>(context)) {
    return state ? ClassifiedFrame(ExternalPriorityState{*state})
                 : ClassifiedFrame(InvalidOrIrrelevant{});
  }
  if (const auto* tail = std::get_if<ClassificationTail>(&context)) {
    if (!state || !tail->window.classifies_at(now_ms)) return InvalidOrIrrelevant{};
    if (state->semantically_equals(tail->expected_state))
      return LocalTailRepeat{*state, tail->epoch_identity};
    return LocalTailContradiction{*state, tail->epoch_identity};
  }
  const auto& active = std::get<ActiveResponseWindow>(context);
  const auto position = active.window.position_at(now_ms);
  if (position == WindowPosition::BeforeAcceptance && state) {
    if (active.window.is_post_command())
      return IgnoredPostCommandPreAcceptanceState{*state};
    return ExternalPriorityState{*state};
  }
  if (position == WindowPosition::ClassificationTail && state) {
    if (active.tracked_state && state->semantically_equals(*active.tracked_state))
      return LocalTailRepeat{*state, active.epoch_identity};
    return LocalTailContradiction{*state, active.epoch_identity};
  }
  if (position != WindowPosition::Accepting) return InvalidOrIrrelevant{};
  const auto recovered = FrameRecovery::recover_response(input, sender);
  if (!recovered) return InvalidOrIrrelevant{};
  if (recovered.value().kind == ResponseKind::Special)
    return SpecialDiagnostic{recovered.value().state};
  return LocalResponseCandidate{recovered.value(), active.epoch_identity};
}

}  // namespace quietcool
