#pragma once

#include "fan_state.h"
#include "sender_id.h"

namespace quietcool {

enum class LearnMode : std::uint8_t { Manual, Automatic };
enum class LearnEventKind : std::uint8_t {
  Ignored, CandidateStarted, CandidateRestarted, Learned, WindowExpired,
  AmbiguousRejected
};
struct LearnEvent final {
  LearnEventKind kind;
  std::optional<SenderId> sender;
};
struct LearnCandidate final {
  SenderId sender;
  MonotonicMs first_ms;
  MonotonicMs last_sighting_ms;
  std::uint8_t sightings;  // saturating count of independent sightings, starts at 1
};
struct LearnSnapshot final {
  bool active;
  LearnMode mode;
  MonotonicMs deadline_ms;
  std::optional<LearnCandidate> candidate;
};

class LearnMachine final {
 public:
  LearnMachine() = default;
  void start(LearnMode mode, MonotonicMs now_ms);
  LearnEvent observe(ByteView input, MonotonicMs now_ms);
  LearnEvent poll(MonotonicMs now_ms);
  void cancel();
  LearnSnapshot snapshot() const;
 private:
  std::optional<SenderId> learnable_sender(ByteView input) const;
  bool active_{false};
  bool ambiguous_{false};
  LearnMode mode_{LearnMode::Manual};
  MonotonicMs deadline_ms_{0};
  std::optional<LearnCandidate> candidate_;
};

}  // namespace quietcool

