#include "learn_machine.h"

namespace quietcool {

void LearnMachine::start(LearnMode mode, MonotonicMs now_ms) {
  active_ = true;
  ambiguous_ = false;
  mode_ = mode;
  deadline_ms_ = saturating_add(now_ms,
      mode == LearnMode::Manual ? 120000U : 900000U);
  candidate_.reset();
}

LearnEvent LearnMachine::observe(ByteView input, MonotonicMs now_ms) {
  if (!active_ || now_ms >= deadline_ms_) return {LearnEventKind::Ignored, std::nullopt};
  const auto sender = learnable_sender(input);
  if (!sender) return {LearnEventKind::Ignored, std::nullopt};
  if (ambiguous_) return {LearnEventKind::Ignored, std::nullopt};
  if (!candidate_) {
    candidate_ = LearnCandidate{*sender, now_ms, now_ms, 1};
    return {LearnEventKind::CandidateStarted, sender};
  }
  if (candidate_->sender != *sender) {
    // A second, distinct learnable fan poisons the window: fail closed rather
    // than binding whichever sender happens to be heard often enough. The
    // previously bound fan (if any) is left untouched by the caller.
    ambiguous_ = true;
    candidate_.reset();
    active_ = false;
    return {LearnEventKind::AmbiguousRejected, sender};
  }
  // Same sender. A frame older than the window span is treated as a fresh start
  // rather than a late sighting of the current candidate.
  if (const auto span = elapsed_since(now_ms, candidate_->first_ms);
      !span || *span >= kLearnWindowSpanMs) {
    candidate_ = LearnCandidate{*sender, now_ms, now_ms, 1};
    return {LearnEventKind::CandidateRestarted, sender};
  }
  // Count independent sightings by the gap since the last counted sighting, so
  // a single OEM self-report burst (frames milliseconds apart) counts once.
  const auto gap = elapsed_since(now_ms, candidate_->last_sighting_ms);
  if (!gap || *gap < kLearnSightingGapMs) return {LearnEventKind::Ignored, std::nullopt};
  candidate_->last_sighting_ms = now_ms;
  if (candidate_->sightings < 0xFF) ++candidate_->sightings;
  if (candidate_->sightings < kLearnMinSightings)
    return {LearnEventKind::Ignored, std::nullopt};
  candidate_.reset();
  active_ = false;
  return {LearnEventKind::Learned, sender};
}

LearnEvent LearnMachine::poll(MonotonicMs now_ms) {
  if (!active_ || now_ms < deadline_ms_)
    return {LearnEventKind::Ignored, std::nullopt};
  cancel();
  return {LearnEventKind::WindowExpired, std::nullopt};
}

void LearnMachine::cancel() {
  active_ = false;
  ambiguous_ = false;
  candidate_.reset();
}

LearnSnapshot LearnMachine::snapshot() const {
  return {active_, mode_, deadline_ms_, candidate_};
}

std::optional<SenderId> LearnMachine::learnable_sender(ByteView input) const {
  if (input.size() != 6 || input[0] != 0xCB || input[4] != input[5])
    return std::nullopt;
  const auto state = FanState::observed(input[4]);
  if (!state || !state.value().has_outbound_command_marker() ||
      !state.value().speed())
    return std::nullopt;
  return SenderId::from_bytes({input[0], input[1], input[2], input[3]}).value();
}

}  // namespace quietcool
