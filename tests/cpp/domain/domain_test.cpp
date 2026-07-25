#include "quietcool/core/fan_state.h"
#include "quietcool/core/sender_id.h"
#include "support/test.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace quietcool {
namespace {

QC_TEST("domain", "sender ID round-trips big-endian wire order") {
  const auto result = SenderId::from_bytes({0xCB, 0x12, 0x34, 0x56});
  QC_CHECK(result.has_value());
  QC_CHECK_EQ(result.value().bytes(),
              (std::array<std::uint8_t, 4>{0xCB, 0x12, 0x34, 0x56}));
  QC_CHECK_EQ(result.value().as_be_u32(), 0xCB123456U);
  QC_CHECK_EQ(SenderId::from_be_u32(0xCB123456U).value(), result.value());
}

QC_TEST("domain", "sender ID rejects every non-operational prefix") {
  for (std::uint16_t prefix = 0; prefix <= 0xFF; ++prefix) {
    const auto result = SenderId::from_bytes(
        {static_cast<std::uint8_t>(prefix), 0x12, 0x34, 0x56});
    QC_CHECK_EQ(result.has_value(), prefix == 0xCB);
  }
  QC_CHECK(!std::is_default_constructible<SenderId>::value);
}

QC_TEST("domain", "speed and duration protocol values are fixed") {
  for (std::uint8_t value = 0; value < 8; ++value) {
    QC_CHECK_EQ(speed_from_value(value).has_value(), value >= 1 && value <= 3);
  }
  for (std::uint8_t value = 0; value < 16; ++value) {
    const bool valid = value == 0 || value == 1 || value == 2 || value == 4 ||
                       value == 8 || value == 12 || value == 15;
    QC_CHECK_EQ(duration_from_value(value).has_value(), valid);
  }
}

QC_TEST("domain", "command states produce the golden matrix") {
  const std::array<Speed, 3> speeds{Speed::Low, Speed::Medium, Speed::High};
  const std::array<Duration, 7> durations{
      Duration::Off,    Duration::Hours1, Duration::Hours2, Duration::Hours4,
      Duration::Hours8, Duration::Hours12, Duration::Continuous};
  for (const auto speed : speeds) {
    for (const auto duration : durations) {
      const auto state = FanState::command(speed, duration);
      const auto golden = static_cast<std::uint8_t>(
          0x80U | (static_cast<std::uint8_t>(speed) << 4U) |
          static_cast<std::uint8_t>(duration));
      QC_CHECK_EQ(state.raw_byte(), golden);
      QC_CHECK_EQ(state.outbound_command_byte(), golden);
    }
  }
}

QC_TEST("domain", "neutral OFF and all OFF variants are semantic equals") {
  const auto neutral = FanState::observed(0xC0);
  QC_CHECK(neutral.has_value());
  QC_CHECK(!neutral.value().speed().has_value());
  for (const auto speed : {Speed::Low, Speed::Medium, Speed::High}) {
    QC_CHECK(neutral.value().semantically_equals(
        FanState::command(speed, Duration::Off)));
  }
}

QC_TEST("domain", "metadata is capability but not canonical identity") {
  const auto unmarked = FanState::observed(0x1F).value();
  const auto one_speed = FanState::observed(0x5F).value();
  const auto command = FanState::observed(0x9F).value();
  const auto three_speed = FanState::observed(0xDF).value();
  QC_CHECK(unmarked.semantically_equals(three_speed));
  QC_CHECK_EQ(unmarked.report_capability().value(), SpeedCapability::Unknown);
  QC_CHECK_EQ(one_speed.report_capability().value(), SpeedCapability::One);
  QC_CHECK(command.has_outbound_command_marker());
  QC_CHECK_EQ(three_speed.report_capability().value(), SpeedCapability::Three);
  QC_CHECK(!FanState::command(Speed::Low, Duration::Continuous)
                .report_capability()
                .has_value());
}

QC_TEST("domain", "invalid duration and zero-speed running are rejected") {
  for (std::uint8_t nibble = 0; nibble < 16; ++nibble) {
    const bool valid = nibble == 0 || nibble == 1 || nibble == 2 ||
                       nibble == 4 || nibble == 8 || nibble == 12 ||
                       nibble == 15;
    QC_CHECK_EQ(FanState::observed(static_cast<std::uint8_t>(0x10 | nibble))
                    .has_value(),
                valid);
  }
  for (const auto duration : {1, 2, 4, 8, 12, 15}) {
    QC_CHECK(!FanState::observed(static_cast<std::uint8_t>(duration)).has_value());
  }
}

QC_TEST("domain", "command rejects cast-invalid nibbles") {
  // A cast-invalid duration keeps a valid speed but an undefined duration nibble.
  QC_CHECK(!FanState::command(Speed::Low, static_cast<Duration>(7))
                .is_valid_command());
  // A cast-invalid speed clears the command marker / zeroes the speed nibble.
  QC_CHECK(!FanState::command(static_cast<Speed>(4), Duration::Continuous)
                .is_valid_command());
  QC_CHECK(!FanState::command(static_cast<Speed>(5), Duration::Off)
                .is_valid_command());
  for (const auto speed : {Speed::Low, Speed::Medium, Speed::High})
    for (const auto duration : {Duration::Off, Duration::Hours1, Duration::Hours2,
                                Duration::Hours4, Duration::Hours8,
                                Duration::Hours12, Duration::Continuous})
      QC_CHECK(FanState::command(speed, duration).is_valid_command());
}

}  // namespace
}  // namespace quietcool

