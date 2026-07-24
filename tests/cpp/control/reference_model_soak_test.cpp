#include "quietcool/core/confirmation_core.h"
#include "support/test.h"
#include "support/test_doubles.h"

#include <optional>
#include <variant>

namespace quietcool {
namespace {

class SoakRandom final {
 public:
  explicit SoakRandom(std::uint32_t value) : value_(value) {}
  std::uint32_t next() {
    value_ = value_ * 1664525U + 1013904223U;
    return value_;
  }
 private:
  std::uint32_t value_;
};

struct ReferenceModel final {
  CoordinatorState state{CoordinatorState::Idle};
  bool sender_exists{true};
  bool transaction_active{false};
  bool oem_priority{false};
  std::uint8_t attempts_started{0};
  std::uint32_t logical_tx_started{0};
};

SenderId soak_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

std::optional<TxRequest> soak_tx(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (const auto* request = std::get_if<RequestTxBurst>(&effects[index]))
      return request->request;
  return std::nullopt;
}

void check_model(const ReferenceModel& model, const ConfirmationCore& core,
                 MonotonicMs now_ms) {
  const auto snapshot = core.snapshot(now_ms);
  QC_CHECK_EQ(snapshot.state, model.state);
  QC_CHECK(ConfirmationCore::context_matches_state(snapshot.state,
                                                   snapshot.context));
  QC_CHECK_EQ(snapshot.transaction.has_value(), model.transaction_active);
  if (snapshot.transaction)
    QC_CHECK_EQ(snapshot.transaction->attempts_started,
                model.attempts_started);
  QC_CHECK_EQ(snapshot.logical_command_bursts + snapshot.logical_query_bursts,
              model.logical_tx_started);
  const bool live_expected =
      model.state == CoordinatorState::CommandLeaseIssued ||
      model.state == CoordinatorState::CommandTransmitting;
  QC_CHECK_EQ(snapshot.live_tx.has_value(), live_expected);
  QC_CHECK_EQ(model.sender_exists,
              model.state != CoordinatorState::Unprovisioned);
  const bool core_oem_priority =
      snapshot.state == CoordinatorState::OemHoldoff ||
      (snapshot.recovery.cause &&
       *snapshot.recovery.cause == RecoveryCause::OemActivity);
  QC_CHECK_EQ(core_oem_priority, model.oem_priority);
}

void restore_model(ReferenceModel& model, bool provisioned) {
  model.state = provisioned ? CoordinatorState::Idle
                            : CoordinatorState::Unprovisioned;
  model.sender_exists = provisioned;
  model.transaction_active = false;
  model.oem_priority = false;
  model.attempts_started = 0;
  model.logical_tx_started = 0;
}

QC_TEST("soak", "FakeClock timelines agree with an independent reference model") {
  SoakRandom random(0xC001D00DU);
  for (std::size_t cycle = 0; cycle < 20000; ++cycle) {
    test::FakeClock clock;
    if ((cycle & 3U) == 0) {
      clock.ingest_raw32(0xFFFFFF00U + random.next() % 128U);
      clock.ingest_raw32(random.next() % 128U);
    } else {
      clock.set(static_cast<MonotonicMs>(random.next()) * 17U);
    }
    const auto base_ms = clock.now_ms();
    ConfirmationCore core(CoreConfig{random.next()});
    RestorableState restored;
    restored.sender = soak_sender();
    core.restore(restored, base_ms);
    ReferenceModel model;
    check_model(model, core, clock.now_ms());

    const bool request_off = random.next() & 1U;
    const auto request = FanState::command(
        request_off ? Speed::Low : Speed::High,
        request_off ? Duration::Off : Duration::Continuous);
    core.request_state(request, clock.now_ms());
    model.state = CoordinatorState::CommandPending;
    model.transaction_active = true;
    check_model(model, core, clock.now_ms());

    if (clock.now_ms() > 0) clock.set(clock.now_ms() - 1);
    core.request_state(request, clock.now_ms());
    check_model(model, core, clock.now_ms());
    clock.set(base_ms);

    const auto lease = soak_tx(core.poll(clock.now_ms()));
    QC_CHECK(lease.has_value());
    model.state = CoordinatorState::CommandLeaseIssued;
    check_model(model, core, clock.now_ms());
    core.on_tx_started(TxToken(lease->token.value() + 1000000U),
                       clock.now_ms());
    check_model(model, core, clock.now_ms());

    switch (random.next() % 6U) {
      case 0: {
        clock.advance(kTxLeaseStartWatchdogMs);
        core.poll(clock.now_ms());
        model.state = CoordinatorState::RadioRecovery;
        check_model(model, core, clock.now_ms());
        clock.advance(1);
        core.on_radio_recovered(clock.now_ms());
        model.state = CoordinatorState::CommandPending;
        check_model(model, core, clock.now_ms());
        break;
      }
      case 1: {
        clock.advance(kTxLeaseStartWatchdogMs);
        core.poll(clock.now_ms());
        model.state = CoordinatorState::RadioRecovery;
        clock.advance(kTxBurstWatchdogMs);
        core.poll(clock.now_ms());
        check_model(model, core, clock.now_ms());
        clock.advance(kTxBurstWatchdogMs);
        core.poll(clock.now_ms());
        model.state = CoordinatorState::Idle;
        model.transaction_active = false;
        check_model(model, core, clock.now_ms());
        break;
      }
      case 2: {
        core.on_tx_started(lease->token, clock.now_ms());
        model.state = CoordinatorState::CommandTransmitting;
        model.attempts_started = 1;
        model.logical_tx_started = 1;
        check_model(model, core, clock.now_ms());
        core.on_tx_started(lease->token, clock.now_ms());
        check_model(model, core, clock.now_ms());
        clock.advance(400);
        core.on_tx_complete(lease->token, clock.now_ms());
        model.state = CoordinatorState::PostCommandListening;
        check_model(model, core, clock.now_ms());
        const auto completion_ms = clock.now_ms();
        clock.set(completion_ms - 1);
        core.poll(clock.now_ms());
        check_model(model, core, clock.now_ms());
        clock.set(completion_ms + kResponseTailEndMs + 1);
        core.poll(clock.now_ms());
        model.state = CoordinatorState::PostCommandTailWait;
        check_model(model, core, clock.now_ms());
        core.poll(clock.now_ms());
        model.state = CoordinatorState::FallbackQueryPending;
        check_model(model, core, clock.now_ms());
        break;
      }
      case 3: {
        core.on_tx_started(lease->token, clock.now_ms());
        model.state = CoordinatorState::CommandTransmitting;
        model.attempts_started = 1;
        model.logical_tx_started = 1;
        clock.advance(kTxBurstWatchdogMs);
        core.poll(clock.now_ms());
        model.state = CoordinatorState::RadioRecovery;
        check_model(model, core, clock.now_ms());
        clock.advance(1);
        core.on_radio_recovered(clock.now_ms());
        model.state = CoordinatorState::ResponseTailQuarantine;
        check_model(model, core, clock.now_ms());
        clock.set(base_ms + kResponseTailEndMs + 1);
        core.poll(clock.now_ms());
        model.state = CoordinatorState::CommandPending;
        check_model(model, core, clock.now_ms());
        break;
      }
      case 4: {
        clock.advance(1);
        const auto oem = FrameCodec::encode_query(soak_sender());
        core.on_frame(ByteView(oem.bytes), clock.now_ms());
        model.state = CoordinatorState::OemHoldoff;
        model.transaction_active = false;
        model.oem_priority = true;
        check_model(model, core, clock.now_ms());
        clock.advance(kOemHoldoffMs - 1);
        core.poll(clock.now_ms());
        check_model(model, core, clock.now_ms());
        clock.advance(1);
        core.poll(clock.now_ms());
        model.state = CoordinatorState::RecoveryQuietWait;
        check_model(model, core, clock.now_ms());
        core.request_state(request, clock.now_ms());
        model.state = CoordinatorState::CommandPending;
        model.transaction_active = true;
        model.oem_priority = false;
        check_model(model, core, clock.now_ms());
        break;
      }
      default:
        core.request_forget(clock.now_ms());
        restore_model(model, false);
        check_model(model, core, clock.now_ms());
        break;
    }

    RestorableState final_restore;
    const bool provisioned = random.next() & 1U;
    if (provisioned) final_restore.sender = soak_sender();
    core.restore(final_restore, clock.now_ms());
    restore_model(model, provisioned);
    check_model(model, core, clock.now_ms());
  }
}

}  // namespace
}  // namespace quietcool
