#include "quietcool/core/frame_codec.h"
#include "support/test.h"

#include <array>
#include <cstdint>
#include <variant>

namespace quietcool {
namespace {

SenderId sender() { return SenderId::from_be_u32(0xCB004739U).value(); }

QC_TEST("protocol", "query uses exact six-byte wire order") {
  QC_CHECK_EQ(FrameCodec::encode_query(sender()).bytes,
              (std::array<std::uint8_t, 6>{0xCB, 0x00, 0x47, 0x39, 0x66,
                                           0x66}));
}

QC_TEST("protocol", "every valid command state round-trips") {
  for (const auto speed : {Speed::Low, Speed::Medium, Speed::High}) {
    for (const auto duration : {Duration::Off, Duration::Hours1,
                                Duration::Hours2, Duration::Hours4,
                                Duration::Hours8, Duration::Hours12,
                                Duration::Continuous}) {
      const auto encoded =
          FrameCodec::encode_state(sender(), FanState::command(speed, duration));
      QC_CHECK(encoded.has_value());
      const auto decoded = FrameCodec::decode_strict(
          ByteView(encoded.value().bytes.data(), encoded.value().bytes.size()),
          sender());
      QC_CHECK(decoded.has_value());
      QC_CHECK(std::holds_alternative<ExactState>(decoded.value()));
      QC_CHECK(std::get<ExactState>(decoded.value())
                   .state.semantically_equals(FanState::command(speed, duration)));
    }
  }
}

QC_TEST("protocol", "encode rejects undefined duration nibble") {
  // Valid speed, cast-invalid duration -> command byte 0x97, which decode would
  // reject; encode must refuse it before it is ever modulated (defence in depth).
  QC_CHECK(!FrameCodec::encode_state(
      sender(), FanState::command(Speed::Low, static_cast<Duration>(7))));
}

QC_TEST("protocol", "encoding normalizes metadata and rejects neutral OFF") {
  const auto observed = FanState::observed(0x5F).value();
  QC_CHECK_EQ(FrameCodec::encode_state(sender(), observed).value().bytes[4], 0x9F);
  QC_CHECK(!FrameCodec::encode_state(sender(), FanState::observed(0xC0).value())
                .has_value());
}

QC_TEST("protocol", "golden observations preserve raw state") {
  for (const auto raw : {0xC0, 0xD1, 0xE2, 0xFF}) {
    const std::array<std::uint8_t, 6> bytes{0xCB, 0x00, 0x47, 0x39,
                                            static_cast<std::uint8_t>(raw),
                                            static_cast<std::uint8_t>(raw)};
    const auto decoded = FrameCodec::decode_strict(ByteView(bytes), sender());
    QC_CHECK(decoded.has_value());
    QC_CHECK_EQ(std::get<ExactState>(decoded.value()).state.raw_byte(), raw);
  }
}

QC_TEST("protocol", "query and special response remain distinct") {
  const auto query = FrameCodec::encode_query(sender());
  const auto decoded_query =
      FrameCodec::decode_strict(ByteView(query.bytes), sender()).value();
  QC_CHECK(std::holds_alternative<ExactQuery>(decoded_query));
  const std::array<std::uint8_t, 6> special{0xCE, 0x00, 0x47, 0x39, 0xDF,
                                            0xDF};
  const auto decoded_special =
      FrameCodec::decode_strict(ByteView(special), sender()).value();
  QC_CHECK(std::holds_alternative<ExactSpecialResponse>(decoded_special));
}

QC_TEST("protocol", "strict decoder rejects lengths sender tails and states") {
  const std::array<std::uint8_t, 7> base{0xCB, 0x00, 0x47, 0x39, 0xDF, 0xDF,
                                        0x00};
  for (const auto length : {0U, 1U, 5U, 7U}) {
    QC_CHECK(!FrameCodec::decode_strict(ByteView(base.data(), length), sender())
                  .has_value());
  }
  auto wrong = base;
  wrong[1] = 0x47;
  QC_CHECK(!FrameCodec::decode_strict(ByteView(wrong.data(), 6), sender())
                .has_value());
  wrong = base;
  wrong[5] = 0xCF;
  QC_CHECK(!FrameCodec::decode_strict(ByteView(wrong.data(), 6), sender())
                .has_value());
  wrong = base;
  wrong[4] = wrong[5] = 0x13;
  QC_CHECK(!FrameCodec::decode_strict(ByteView(wrong.data(), 6), sender())
                .has_value());
  wrong = {0xCE, 0x00, 0x47, 0x39, 0x66, 0x66, 0x00};
  QC_CHECK(!FrameCodec::decode_strict(ByteView(wrong.data(), 6), sender())
                .has_value());
}

QC_TEST("protocol", "metadata masking never changes sender bytes") {
  const auto frame = FrameCodec::encode_state(sender(), FanState::observed(0x5F).value());
  QC_CHECK_EQ(frame.value().bytes[0], 0xCB);
  QC_CHECK_EQ(frame.value().bytes[1], 0x00);
  QC_CHECK_EQ(frame.value().bytes[2], 0x47);
  QC_CHECK_EQ(frame.value().bytes[3], 0x39);
}

}  // namespace
}  // namespace quietcool

