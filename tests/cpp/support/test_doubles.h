#pragma once

#include "quietcool/ports/clock.h"
#include "quietcool/ports/event_sink.h"
#include "quietcool/ports/radio.h"
#include "quietcool/radio/burst_transmitter.h"

#include <optional>
#include <vector>

namespace quietcool::test {

class FakeClock final : public Clock {
 public:
  MonotonicMs now_ms() const override { return now_ms_; }
  void set(MonotonicMs now_ms) { now_ms_ = now_ms; }
  void advance(MonotonicMs delta) { now_ms_ = saturating_add(now_ms_, delta); }
  void ingest_raw32(std::uint32_t raw) {
    if (raw_seen_ && raw < last_raw_) raw_epoch_ += MonotonicMs{1} << 32U;
    raw_seen_ = true;
    last_raw_ = raw;
    now_ms_ = raw_epoch_ + raw;
  }
 private:
  MonotonicMs now_ms_{0};
  MonotonicMs raw_epoch_{0};
  std::uint32_t last_raw_{0};
  bool raw_seen_{false};
};

class FakeRadio final : public Radio {
 public:
  RadioSendResult send_packet(const FrameBytes& payload) override {
    packets_.push_back(payload);
    if (next_result_ == results_.size()) return RadioSendResult::Sent;
    return results_[next_result_++];
  }
  RadioRecoveryResult recover() override {
    ++recovery_count_;
    return recovery_result_;
  }
  void push_result(RadioSendResult result) { results_.push_back(result); }
  void set_recovery_result(RadioRecoveryResult result) {
    recovery_result_ = result;
  }
  const std::vector<FrameBytes>& packets() const { return packets_; }
  std::size_t recovery_count() const { return recovery_count_; }
 private:
  std::vector<FrameBytes> packets_;
  std::vector<RadioSendResult> results_;
  std::size_t next_result_{0};
  std::size_t recovery_count_{0};
  RadioRecoveryResult recovery_result_{RadioRecoveryResult::Recovered};
};

class RecordingEventSink final : public EventSink {
 public:
  void on_core_event(const CoreEvent& event) override { events_.push_back(event); }
  const std::vector<CoreEvent>& events() const { return events_; }
 private:
  std::vector<CoreEvent> events_;
};

class FakeBurstTransmitter final {
 public:
  TxAcceptResult accept(const TxRequest& request) {
    if (request_) return TxAcceptResult::Busy;
    request_ = request;
    return TxAcceptResult::Accepted;
  }
  BurstEvent started() { return remember(BurstStarted{request_->token}); }
  BurstEvent completed() {
    const auto event = remember(BurstComplete{request_->token});
    request_.reset();
    return event;
  }
  BurstEvent rejected() {
    const auto event = remember(BurstRejected{request_->token});
    request_.reset();
    return event;
  }
  BurstEvent faulted() { return remember(BurstFault{request_->token}); }
  BurstEvent duplicate_last() const { return *last_event_; }
  BurstEvent stale_started(TxToken token) const { return BurstStarted{token}; }
  BurstEvent stale_complete(TxToken token) const { return BurstComplete{token}; }
  std::optional<TxToken> watchdog_token() const {
    return request_ ? std::optional<TxToken>(request_->token) : std::nullopt;
  }
 private:
  template <typename Event>
  BurstEvent remember(Event event) {
    last_event_ = BurstEvent(event);
    return *last_event_;
  }
  std::optional<TxRequest> request_;
  std::optional<BurstEvent> last_event_;
};

class FakePreferences final {
 public:
  RestorableState load() const { return restored_; }
  void apply(const PersistenceRequest& request) {
    if (request.kind == PersistenceKind::SaveProvisioning) {
      restored_.sender = request.sender;
    } else if (request.kind == PersistenceKind::EraseProvisioning) {
      restored_.sender.reset();
      restored_.remembered_speed.reset();
      restored_.observation_hint.reset();
      restored_.speed_capability.reset();
      restored_.seed_policy = SeedPolicy::SuppressCompiledSeed;
    } else if (request.kind == PersistenceKind::SaveRememberedSpeed) {
      restored_.remembered_speed = request.remembered_speed;
    } else if (request.kind == PersistenceKind::SaveSpeedCapability) {
      restored_.speed_capability = request.speed_capability;
    }
  }
  void sync() { ++sync_count_; }
  std::size_t sync_count() const { return sync_count_; }
 private:
  RestorableState restored_;
  std::size_t sync_count_{0};
};

}  // namespace quietcool::test
