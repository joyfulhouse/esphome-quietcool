#pragma once

#include "core_types.h"

namespace quietcool {

struct PostCommandEpoch final { TransactionId transaction; AttemptNumber attempt; };
struct BootQueryEpoch final { TxToken token; };
struct ManualQueryEpoch final { TxToken token; };
struct HeartbeatQueryEpoch final { TxToken token; };
struct FallbackQueryEpoch final { TxToken token; };
struct RecoveryQueryEpoch final { TxToken token; };
struct TailEpoch final { MonotonicMs anchor_ms; };
using ResponseEpoch = std::variant<PostCommandEpoch, BootQueryEpoch,
    ManualQueryEpoch, HeartbeatQueryEpoch, FallbackQueryEpoch,
    RecoveryQueryEpoch, TailEpoch>;

enum class WindowPosition : std::uint8_t {
  BeforeAcceptance, Accepting, ClassificationTail, Expired
};

class ResponseWindow final {
 public:
  static ResponseWindow post_command(TransactionId transaction,
                                     AttemptNumber attempt,
                                     MonotonicMs burst_completed_ms);
  static ResponseWindow query(QueryPurpose purpose, TxToken token,
                              MonotonicMs burst_started_ms);
  static ResponseWindow classification_tail(MonotonicMs anchor_ms);
  WindowPosition position_at(MonotonicMs now_ms) const;
  bool accepts_at(MonotonicMs now_ms) const;
  bool classifies_at(MonotonicMs now_ms) const;
  const ResponseEpoch& epoch() const { return epoch_; }
  MonotonicMs anchor_ms() const { return anchor_ms_; }
  bool is_post_command() const;
 private:
  ResponseWindow(ResponseEpoch epoch, MonotonicMs anchor, MonotonicMs start,
                 MonotonicMs end, MonotonicMs tail)
      : epoch_(std::move(epoch)), anchor_ms_(anchor), accept_start_(start),
        accept_end_(end), tail_end_(tail) {}
  ResponseEpoch epoch_;
  MonotonicMs anchor_ms_;
  MonotonicMs accept_start_;
  MonotonicMs accept_end_;
  MonotonicMs tail_end_;
};

}  // namespace quietcool
