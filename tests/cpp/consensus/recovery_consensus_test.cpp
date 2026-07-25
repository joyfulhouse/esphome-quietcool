#include "quietcool/core/consensus_tracker.h"
#include "quietcool/core/frame_recovery.h"
#include "support/test.h"

#include <array>
#include <cstdint>

namespace quietcool {
namespace {

SenderId test_sender() { return SenderId::from_be_u32(0xCB004739U).value(); }

RecoveredResponse response(std::uint8_t header, std::uint8_t state,
                           std::size_t length = 6) {
  const std::array<std::uint8_t, 7> bytes{header, 0x00, 0x47, 0x39, state,
                                          state, 0xAA};
  return FrameRecovery::recover_response(ByteView(bytes.data(), length),
                                          test_sender())
      .value();
}

QC_TEST("recovery", "exact normal and special responses remain distinct") {
  QC_CHECK_EQ(response(0xCB, 0xDF).quality, RecoveryQuality::Exact);
  QC_CHECK_EQ(response(0xCB, 0xDF).kind, ResponseKind::Normal);
  QC_CHECK_EQ(response(0xCE, 0xDF).quality, RecoveryQuality::Exact);
  QC_CHECK_EQ(response(0xCE, 0xDF).kind, ResponseKind::Special);
}

QC_TEST("recovery", "all first bytes obey the bounded recovery rule") {
  for (std::uint16_t header = 0; header <= 0xFF; ++header) {
    const std::array<std::uint8_t, 6> bytes{
        static_cast<std::uint8_t>(header), 0x00, 0x47, 0x39, 0xDF, 0xDF};
    const auto result = FrameRecovery::recover_response(ByteView(bytes), test_sender());
    const auto h = static_cast<std::uint8_t>(header);
    const auto distance_cb = static_cast<unsigned>(__builtin_popcount(h ^ 0xCB));
    const auto distance_ce = static_cast<unsigned>(__builtin_popcount(h ^ 0xCE));
    const bool valid = h == 0xCB || h == 0xCE ||
                       (distance_cb <= 1 && distance_cb < distance_ce);
    QC_CHECK_EQ(result.has_value(), valid);
  }
}

QC_TEST("recovery", "invalid shapes and query tails are rejected") {
  const std::array<std::uint8_t, 7> good{0xCB, 0x00, 0x47, 0x39, 0xDF, 0xDF,
                                        0x00};
  QC_CHECK(!FrameRecovery::recover_response(ByteView(good.data(), 5), test_sender())
                .has_value());
  auto bad = good;
  bad[3] = 0x38;
  QC_CHECK(!FrameRecovery::recover_response(ByteView(bad.data(), 6), test_sender())
                .has_value());
  bad = good;
  bad[5] = 0xCF;
  QC_CHECK(!FrameRecovery::recover_response(ByteView(bad.data(), 6), test_sender())
                .has_value());
  bad = good;
  bad[4] = bad[5] = 0x13;
  QC_CHECK(!FrameRecovery::recover_response(ByteView(bad.data(), 6), test_sender())
                .has_value());
  bad = good;
  bad[4] = bad[5] = 0x66;
  QC_CHECK(!FrameRecovery::recover_response(ByteView(bad.data(), 6), test_sender())
                .has_value());
}

QC_TEST("recovery", "overlength is bounded and marked recovered") {
  const auto recovered = response(0xCB, 0xDF, 7);
  QC_CHECK_EQ(recovered.quality, RecoveryQuality::Recovered);
  QC_CHECK_EQ(recovered.state.raw_byte(), 0xDF);
}

QC_TEST("consensus", "one exact plus an independent match reaches consensus") {
  ConsensusTracker tracker;
  QC_CHECK(!tracker.observe(response(0xCB, 0x5F), 100).has_value());
  const auto consensus = tracker.observe(response(0xC9, 0xDF), 160);
  QC_CHECK(consensus.has_value());
  QC_CHECK_EQ(consensus->state.raw_byte(), 0xDF);
  QC_CHECK_EQ(consensus->confidence, EvidenceConfidence::ExactBackedConsensus);
  QC_CHECK_EQ(consensus->independent_candidates, 2);
  QC_CHECK_EQ(consensus->capability, SpeedCapability::Three);
}

QC_TEST("consensus", "recovered-only consensus requires three independent candidates") {
  ConsensusTracker tracker;
  QC_CHECK(!tracker.observe(response(0xC9, 0x1F), 0).has_value());
  QC_CHECK(!tracker.observe(response(0xC9, 0x1F), 60).has_value());
  const auto consensus = tracker.observe(response(0xC9, 0x1F), 120);
  QC_CHECK(consensus.has_value());
  QC_CHECK_EQ(consensus->confidence, EvidenceConfidence::RecoveredOnlyConsensus);
}

QC_TEST("consensus", "59 ms is debounced and exactly 60 ms is independent") {
  ConsensusTracker tracker;
  tracker.observe(response(0xCB, 0x1F), 100);
  QC_CHECK(!tracker.observe(response(0xCB, 0x5F), 159).has_value());
  QC_CHECK(!tracker.observe(response(0xCB, 0x5F), 218).has_value());
  const auto consensus = tracker.observe(response(0xCB, 0xDF), 278);
  QC_CHECK(consensus.has_value());
  QC_CHECK_EQ(consensus->capability, SpeedCapability::Three);
}

QC_TEST("consensus", "backward time cannot rebase candidate timing") {
  ConsensusTracker tracker;
  tracker.observe(response(0xCB, 0x1F), 100);
  QC_CHECK(!tracker.observe(response(0xCB, 0x1F), 90).has_value());
  QC_CHECK(tracker.observe(response(0xCB, 0x1F), 160).has_value());
}

QC_TEST("consensus", "canonical change resets all group facts") {
  ConsensusTracker tracker;
  tracker.observe(response(0xCB, 0x5F), 0);
  tracker.observe(response(0xC9, 0x5F), 60);
  QC_CHECK(!tracker.observe(response(0xC9, 0x2F), 120).has_value());
  const auto snapshot = tracker.snapshot();
  QC_CHECK(snapshot.has_group);
  QC_CHECK_EQ(snapshot.canonical_byte, 0x2F);
  QC_CHECK_EQ(snapshot.independent_candidates, 1);
  QC_CHECK(!snapshot.has_exact);
}

QC_TEST("consensus", "special responses do not participate or reset") {
  ConsensusTracker tracker;
  tracker.observe(response(0xCB, 0x1F), 0);
  QC_CHECK(!tracker.observe(response(0xCE, 0x2F), 60).has_value());
  QC_CHECK(tracker.observe(response(0xCB, 0x1F), 60).has_value());
  tracker.reset();
  QC_CHECK(!tracker.snapshot().has_group);
}

}  // namespace
}  // namespace quietcool
