#include "learn_machine.h"

namespace quietcool {

void LearnMachine::start(LearnMode mode, MonotonicMs now_ms) {
  active_ = true;
  mode_ = mode;
  deadline_ms_ = saturating_add(now_ms,
      mode == LearnMode::Manual ? 120000U : 900000U);
  candidate_.reset();
}

LearnEvent LearnMachine::observe(ByteView input, MonotonicMs now_ms) {
  if (!active_ || now_ms >= deadline_ms_) return {LearnEventKind::Ignored, std::nullopt};
  const auto sender = learnable_sender(input);
  if (!sender) return {LearnEventKind::Ignored, std::nullopt};
  if (!candidate_) {
    candidate_ = LearnCandidate{*sender, now_ms};
    return {LearnEventKind::CandidateStarted, sender};
  }
  if (candidate_->sender != *sender) {
    candidate_ = LearnCandidate{*sender, now_ms};
    return {LearnEventKind::CandidateRestarted, sender};
  }
  const auto age = elapsed_since(now_ms, candidate_->started_ms);
  if (!age || *age <= 600) return {LearnEventKind::Ignored, std::nullopt};
  if (*age < 60000) {
    candidate_.reset();
    active_ = false;
    return {LearnEventKind::Learned, sender};
  }
  candidate_ = LearnCandidate{*sender, now_ms};
  return {LearnEventKind::CandidateRestarted, sender};
}

LearnEvent LearnMachine::poll(MonotonicMs now_ms) {
  if (!active_ || now_ms < deadline_ms_)
    return {LearnEventKind::Ignored, std::nullopt};
  cancel();
  return {LearnEventKind::WindowExpired, std::nullopt};
}

void LearnMachine::cancel() {
  active_ = false;
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
