#include "quietcool/core/confirmation_core.h"
#include "support/test.h"
#include "support/test_doubles.h"

#include <algorithm>
#include <array>
#include <optional>
#include <variant>

namespace quietcool {
namespace {

class SequenceRandom final {
 public:
  explicit SequenceRandom(std::uint32_t seed) : value_(seed) {}
  std::uint32_t next() {
    value_ ^= value_ << 13U;
    value_ ^= value_ >> 17U;
    value_ ^= value_ << 5U;
    return value_;
  }
 private:
  std::uint32_t value_;
};

SenderId sequence_sender() {
  return SenderId::from_be_u32(0xCB004739U).value();
}

FrameBytes sequence_frame(std::uint8_t state) {
  return {{0xCB, 0x00, 0x47, 0x39, state, state}};
}

std::size_t sequence_tx_count(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < effects.size(); ++index)
    count += std::holds_alternative<RequestTxBurst>(effects[index]);
  return count;
}

bool legal_authority(const AuthoritySnapshot& authority) {
  if (std::holds_alternative<UnknownStateAuthority>(authority.state) ||
      std::holds_alternative<RevalidatingStateAuthority>(authority.state))
    return std::holds_alternative<UnknownTimerAuthority>(authority.timer);
  const auto& state = std::get<ConfirmedStateAuthority>(authority.state).state;
  if (state.duration() == Duration::Off ||
      state.duration() == Duration::Continuous)
    return std::holds_alternative<NoActiveTimerAuthority>(authority.timer);
  return std::holds_alternative<ProgrammedDurationAuthority>(authority.timer) ||
      std::holds_alternative<LocallyAnchoredTimerAuthority>(authority.timer);
}

bool operational_for_oem(CoordinatorState state) {
  return state != CoordinatorState::Unprovisioned &&
      state != CoordinatorState::LearningAwaitingFirst &&
      state != CoordinatorState::LearningAwaitingSecond;
}

void check_live_tx_unchanged(const CoreSnapshot& before,
                             const CoreSnapshot& after) {
  QC_CHECK_EQ(after.live_tx.has_value(), before.live_tx.has_value());
  if (!before.live_tx) return;
  QC_CHECK_EQ(after.live_tx->token, before.live_tx->token);
  QC_CHECK_EQ(after.live_tx->reason, before.live_tx->reason);
  QC_CHECK_EQ(after.live_tx->payload, before.live_tx->payload);
}

QC_TEST("generated", "arbitrary public event sequences preserve core invariants") {
  for (std::uint32_t seed = 1; seed <= 32; ++seed) {
    SequenceRandom random(seed * 0x9E3779B9U);
    test::FakeClock clock;
    ConfirmationCore core(CoreConfig{seed});
    RestorableState initial;
    initial.sender = sequence_sender();
    core.restore(initial, 0);

    for (std::size_t step = 0; step < 1000; ++step) {
      const auto timing = random.next();
      if ((timing & 7U) == 0 && clock.now_ms() > 0) {
        const auto backward = std::min<MonotonicMs>(clock.now_ms(),
                                                    timing % 250U);
        clock.set(clock.now_ms() - backward);
      } else {
        clock.advance(timing % 7001U);
      }
      const auto now_ms = clock.now_ms();
      const auto before = core.snapshot(now_ms);
      CoreEffects effects;
      bool was_poll = false;
      bool malformed = false;
      bool exact_oem = false;
      bool stale_callback = false;
      bool restore_event = false;
      bool restore_valid_sender = false;
      bool refresh_event = false;

      switch (random.next() % 16U) {
        case 0: {
          RestorableState restored;
          const auto kind = random.next() % 3U;
          if (kind == 0) restored.sender = sequence_sender();
          if (kind == 2) {
            restored.version = kRestoreSchemaVersion + 1;
            restored.sender = sequence_sender();
          }
          restore_event = true;
          restore_valid_sender = kind == 0;
          effects = core.restore(restored, now_ms);
          break;
        }
        case 1: effects = core.on_radio_ready(now_ms); break;
        case 2: {
          const auto speed = static_cast<Speed>(1U + random.next() % 3U);
          const std::array<Duration, 3> durations{
              Duration::Off, Duration::Hours1, Duration::Continuous};
          effects = core.request_state(
              FanState::command(speed, durations[random.next() % 3U]), now_ms);
          break;
        }
        case 3:
          refresh_event = true;
          effects = core.request_manual_refresh(now_ms);
          break;
        case 4:
          effects = core.request_learn(
              random.next() & 1U ? LearnMode::Manual : LearnMode::Automatic,
              now_ms);
          break;
        case 5: effects = core.request_forget(now_ms); break;
        case 6: {
          const auto query = FrameCodec::encode_query(sequence_sender());
          exact_oem = true;
          effects = core.on_frame(ByteView(query.bytes), now_ms);
          break;
        }
        case 7: {
          const std::array<std::uint8_t, 6> states{
              0xC0, 0x9F, 0xBF, 0xDF, 0xD1, 0xFF};
          const auto frame = sequence_frame(states[random.next() % states.size()]);
          effects = core.on_frame(ByteView(frame.bytes), now_ms);
          break;
        }
        case 8: {
          const std::array<std::uint8_t, 3> bytes{0xCB, 0x00, 0x47};
          malformed = true;
          effects = core.on_frame(ByteView(bytes), now_ms);
          break;
        }
        case 9: {
          const bool matching = before.live_tx && (random.next() & 1U);
          const auto token = matching ? before.live_tx->token
                                      : TxToken(100000U + random.next());
          stale_callback = !matching;
          effects = core.on_tx_started(token, now_ms);
          break;
        }
        case 10: {
          const bool matching = before.live_tx && (random.next() & 1U);
          const auto token = matching ? before.live_tx->token
                                      : TxToken(200000U + random.next());
          stale_callback = !matching;
          effects = core.on_tx_complete(token, now_ms);
          break;
        }
        case 11: {
          const bool matching = before.live_tx && (random.next() & 1U);
          const auto token = matching ? before.live_tx->token
                                      : TxToken(300000U + random.next());
          stale_callback = !matching;
          effects = core.on_tx_rejected(token, now_ms);
          break;
        }
        case 12: effects = core.on_radio_recovered(now_ms); break;
        case 13:
        case 14:
          was_poll = true;
          effects = core.poll(now_ms);
          break;
        default: {
          const auto command = sequence_frame(0x9F);
          effects = core.on_frame(ByteView(command.bytes), now_ms);
          break;
        }
      }

      const auto after = core.snapshot(now_ms);
      QC_CHECK(effects.size() <= 8U);
      QC_CHECK(ConfirmationCore::context_matches_state(after.state,
                                                       after.context));
      QC_CHECK(legal_authority(after.authority));
      QC_CHECK_EQ(sequence_tx_count(effects) <= 1U, true);
      if (after.transaction) {
        QC_CHECK(after.transaction->attempts_started <=
                 after.transaction->attempt_limit);
        QC_CHECK(after.transaction->attempt_limit == 4U ||
                 after.transaction->attempt_limit == 6U);
      }
      if (after.state == CoordinatorState::Unprovisioned)
        QC_CHECK_EQ(sequence_tx_count(effects), 0U);
      if (sequence_tx_count(effects) == 1U) {
        QC_CHECK(was_poll);
        const auto permission = TransitionTable::rf_permission(before.state);
        QC_CHECK(permission.lease_command || permission.lease_query);
        QC_CHECK(after.live_tx.has_value());
        QC_CHECK(after.live_tx->token.value() != 0U);
      }
      const bool tx_state =
          after.state == CoordinatorState::CommandLeaseIssued ||
          after.state == CoordinatorState::CommandTransmitting ||
          after.state == CoordinatorState::BootQueryLeaseIssued ||
          after.state == CoordinatorState::BootQueryTransmitting ||
          after.state == CoordinatorState::ManualQueryLeaseIssued ||
          after.state == CoordinatorState::ManualQueryTransmitting ||
          after.state == CoordinatorState::FallbackQueryLeaseIssued ||
          after.state == CoordinatorState::FallbackQueryTransmitting ||
          after.state == CoordinatorState::RecoveryQueryLeaseIssued ||
          after.state == CoordinatorState::RecoveryQueryTransmitting;
      QC_CHECK_EQ(after.live_tx.has_value(), tx_state);

      if (stale_callback) {
        QC_CHECK_EQ(after.state, before.state);
        QC_CHECK_EQ(after.authority.revision, before.authority.revision);
        check_live_tx_unchanged(before, after);
        QC_CHECK_EQ(after.transaction.has_value(), before.transaction.has_value());
        if (before.transaction)
          QC_CHECK_EQ(after.transaction->attempts_started,
                      before.transaction->attempts_started);
      }
      if (malformed) {
        QC_CHECK_EQ(after.state, before.state);
        QC_CHECK_EQ(after.authority.revision, before.authority.revision);
      }
      if (refresh_event && before.state != CoordinatorState::Idle) {
        QC_CHECK_EQ(after.state, before.state);
        QC_CHECK_EQ(sequence_tx_count(effects), 0U);
        QC_CHECK_EQ(after.authority.revision, before.authority.revision);
      }
      if (exact_oem && operational_for_oem(before.state))
        QC_CHECK_EQ(after.state, CoordinatorState::OemHoldoff);
      if (restore_event)
        QC_CHECK_EQ(after.state, restore_valid_sender ? CoordinatorState::Idle
                                                      : CoordinatorState::Unprovisioned);
    }
  }
}

}  // namespace
}  // namespace quietcool
