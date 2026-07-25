#include "response_classifier.h"

namespace quietcool {
namespace {

// A single, decode-once resolution of an incoming frame. Error tolerance must
// be a property of decoding, not of which classifier branch happens to be
// executing, so every branch below consumes this one result. decode_strict is
// tried first, which keeps an exact frame's exact semantics — in particular the
// ExactQuery / SpecialQuery split that recover_response collapses into a single
// RecoveryError::Query. recover_response then supplies the field-observed
// tolerance (a corrupted length byte -> overlength buffer, and a single-bit hit
// on the header) whenever strict decoding fails on length or a recoverable
// header. Both decoders require bytes[1..3] to match our sender exactly, so no
// tolerance here can ever admit another unit on air.
struct ResolvedFrame final {
  bool is_query = false;                    // a 0x66 query addressed to us
  std::optional<RecoveredResponse> report;  // otherwise a state/special report
};

ResolvedFrame resolve_frame(ByteView input, SenderId sender) {
  const auto strict = FrameCodec::decode_strict(input, sender);
  if (strict) {
    const auto& decoded = strict.value();
    if (std::holds_alternative<ExactQuery>(decoded)) return {true, std::nullopt};
    if (const auto* normal = std::get_if<ExactState>(&decoded))
      return {false, RecoveredResponse{RecoveryQuality::Exact,
                                       ResponseKind::Normal, normal->state}};
    if (const auto* special = std::get_if<ExactSpecialResponse>(&decoded))
      return {false, RecoveredResponse{RecoveryQuality::Exact,
                                       ResponseKind::Special, special->state}};
  }
  const auto recovered = FrameRecovery::recover_response(input, sender);
  if (recovered) return {false, recovered.value()};
  // A 0x66 tail recovers as a query, never a state (RecoveryError::Query). A
  // NORMAL-header query is heard as OEM activity exactly as an exact one is, so
  // a corrupted OEM query still asserts priority. A SPECIAL-header (0xCE) query
  // stays unheard, preserving decode_strict's deliberate SpecialQuery-vs-
  // ExactQuery split: 0xCE semantics are uncharacterised and must never be an
  // authority input (the same reason SpecialDiagnostic is log-only), and
  // assert_oem_priority would invalidate authority. Reading input[0] is safe:
  // RecoveryError::Query is only returned after input[0] and input[4] were
  // read, so a Query error implies size >= 6. Every other recovery error means
  // "not ours" or "uncorrectable" and stays unheard.
  const bool normal_query =
      recovered.error() == RecoveryError::Query && input[0] != 0xCE;
  return {normal_query, std::nullopt};
}

}  // namespace

ClassifiedFrame ResponseClassifier::classify(ByteView input, SenderId sender,
                                             const ReceiveContext& context,
                                             MonotonicMs now_ms) const {
  const auto resolved = resolve_frame(input, sender);
  if (resolved.is_query) return ExactOemQuery{};
  // A 0xCE special response is diagnostic-only in every context, exactly as the
  // strict and in-window paths this unifies already treated it.
  if (resolved.report && resolved.report->kind == ResponseKind::Special)
    return SpecialDiagnostic{resolved.report->state};
  const std::optional<FanState> state =
      resolved.report ? std::optional<FanState>(resolved.report->state)
                      : std::nullopt;

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
  if (position != WindowPosition::Accepting || !resolved.report)
    return InvalidOrIrrelevant{};
  return LocalResponseCandidate{*resolved.report, active.epoch_identity};
}

}  // namespace quietcool
