#pragma once

#include "esphome/components/quietcool/core/core_types.h"
#include "esphome/components/quietcool/ports/clock.h"
#include "esphome/components/quietcool/ports/radio.h"

namespace quietcool {

enum class BurstPhase : std::uint8_t {
  Idle, Accepted, SendingFrame1, Gap1, SendingFrame2, Gap2, SendingFrame3,
  Complete, Fault
};
enum class TxAcceptResult : std::uint8_t { Accepted, Busy };
struct BurstStarted final { TxToken token; };
struct BurstComplete final { TxToken token; };
struct BurstRejected final { TxToken token; };
struct BurstFault final { TxToken token; };
using BurstEvent = std::variant<BurstStarted, BurstComplete, BurstRejected,
                                BurstFault>;
struct BurstSnapshot final {
  BurstPhase phase;
  std::optional<TxToken> token;
  std::uint8_t frames_sent;
  std::optional<MonotonicMs> next_frame_ms;
};

class BurstTransmitter final {
 public:
  BurstTransmitter(Clock& clock, Radio& radio) : clock_(clock), radio_(radio) {}
  TxAcceptResult accept(const TxRequest& request);
  std::optional<BurstEvent> poll();
  std::optional<BurstEvent> poll(MonotonicMs now_ms);
  bool revoke_if_unstarted(TxToken token);
  BurstSnapshot snapshot() const;
 private:
  std::optional<BurstEvent> send_next(MonotonicMs now_ms);
  void clear();
  Clock& clock_;
  Radio& radio_;
  std::optional<TxRequest> request_;
  BurstPhase phase_{BurstPhase::Idle};
  std::uint8_t frames_sent_{0};
  std::optional<MonotonicMs> next_frame_ms_;
};

}  // namespace quietcool
