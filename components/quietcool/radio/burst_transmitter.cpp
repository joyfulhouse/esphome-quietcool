#include "burst_transmitter.h"

namespace quietcool {

TxAcceptResult BurstTransmitter::accept(const TxRequest& request) {
  if (phase_ != BurstPhase::Idle) return TxAcceptResult::Busy;
  request_ = request;
  phase_ = BurstPhase::Accepted;
  frames_sent_ = 0;
  next_frame_ms_.reset();
  return TxAcceptResult::Accepted;
}

std::optional<BurstEvent> BurstTransmitter::poll() {
  return poll(clock_.now_ms());
}

std::optional<BurstEvent> BurstTransmitter::poll(MonotonicMs now_ms) {
  if (phase_ == BurstPhase::Complete || phase_ == BurstPhase::Fault) {
    clear();
    return std::nullopt;
  }
  if (phase_ == BurstPhase::Accepted) {
    phase_ = BurstPhase::SendingFrame1;
    return BurstStarted{request_->token};
  }
  if (phase_ == BurstPhase::SendingFrame1 && frames_sent_ == 0)
    return send_next(now_ms);
  if ((phase_ == BurstPhase::Gap1 || phase_ == BurstPhase::Gap2) &&
      next_frame_ms_ && now_ms >= *next_frame_ms_)
    return send_next(now_ms);
  return std::nullopt;
}

bool BurstTransmitter::revoke_if_unstarted(TxToken token) {
  if (phase_ != BurstPhase::Accepted || !request_ || request_->token != token)
    return false;
  clear();
  return true;
}

BurstSnapshot BurstTransmitter::snapshot() const {
  return {phase_, request_ ? std::optional<TxToken>(request_->token)
                           : std::nullopt,
          frames_sent_, next_frame_ms_};
}

// INVARIANT: at most one synchronous transmit per loop iteration.
// radio_.send_packet() maps to the upstream SX127x::transmit_packet, which
// busy-waits on DIO0 for up to 4000 ms with no yield and no watchdog feed. The
// loop task's watchdog is 5000 ms (panic), fed once per iteration, so a single
// stuck transmit stalls the loop up to ~4 s but leaves ~1 s of margin — a
// liveness hit to API/OTA, not a reboot. send_next() issues exactly one
// send_packet per call, and poll() calls it at most once per iteration (the
// next frame is gated behind kInterFrameGapMs into a later iteration). Do not
// issue a second synchronous transmit in one iteration, and do not lower the
// watchdog below the transmit timeout: either turns the stall into a panic
// reboot. See docs/hardware.md, "Transmit timing and the task watchdog", and
// issue #13.
std::optional<BurstEvent> BurstTransmitter::send_next(MonotonicMs now_ms) {
  if (!request_) return std::nullopt;
  phase_ = frames_sent_ == 0 ? BurstPhase::SendingFrame1
      : frames_sent_ == 1 ? BurstPhase::SendingFrame2
                          : BurstPhase::SendingFrame3;
  const auto result = radio_.send_packet(request_->payload);
  if (result != RadioSendResult::Sent) {
    phase_ = BurstPhase::Fault;
    return BurstFault{request_->token};
  }
  ++frames_sent_;
  if (frames_sent_ == 3) {
    phase_ = BurstPhase::Complete;
    next_frame_ms_.reset();
    return BurstComplete{request_->token};
  }
  next_frame_ms_ = saturating_add(now_ms, kInterFrameGapMs);
  phase_ = frames_sent_ == 1 ? BurstPhase::Gap1 : BurstPhase::Gap2;
  return std::nullopt;
}

void BurstTransmitter::clear() {
  request_.reset();
  phase_ = BurstPhase::Idle;
  frames_sent_ = 0;
  next_frame_ms_.reset();
}

}  // namespace quietcool
