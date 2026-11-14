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
  // The two frames agree on the STATE (same canonical byte) but name different
  // fan-only capabilities, one vote each. This used to report the later
  // frame's; a split is now no capability at all (issue #46 follow-up).
  QC_CHECK_EQ(consensus->capability, SpeedCapability::Unknown);
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

// Issue #31 review: a 2-speed fan's confirming report is byte-identical to a
// command frame — both carry marker bits 10 — so the receiver cannot tell them
// apart. It must therefore RANK the evidence, not drop it: dropping it would
// make capability Two unlearnable, so a 2-speed fan would never confirm or
// persist its band and every boot would fall back to the conservative
// unknown-capability assumption.
QC_TEST("consensus", "a command-shaped candidate still teaches the band") {
  ConsensusTracker tracker;
  QC_CHECK(!tracker.observe(response(0xCB, 0x9F), 100).has_value());
  const auto consensus = tracker.observe(response(0xCB, 0x9F), 160);
  QC_CHECK(consensus.has_value());
  QC_CHECK_EQ(consensus->capability, SpeedCapability::Two);
  QC_CHECK_EQ(consensus->capability_evidence,
              CapabilityEvidence::CommandShaped);
}

// ...but that evidence is the WEAKEST kind. A 3-speed fan answers a 0xAF
// command with 0xEF: a different raw byte, the same canonical byte, so it
// joins the very same group. Its Three must beat the command's aliased Two
// whichever frame lands first — otherwise arrival order decides the fan's
// band.
QC_TEST("consensus", "a fan-only capability outranks a command alias in both orders") {
  {
    ConsensusTracker tracker;  // command first, genuine second
    tracker.observe(response(0xCB, 0xAF), 100);
    const auto consensus = tracker.observe(response(0xCB, 0xEF), 160);
    QC_CHECK(consensus.has_value());
    QC_CHECK_EQ(consensus->capability, SpeedCapability::Three);
    // One fan frame names the band but is not corroboration (issue #46).
    QC_CHECK_EQ(consensus->capability_evidence,
                CapabilityEvidence::Uncorroborated);
  }
  {
    ConsensusTracker tracker;  // genuine first, command second
    tracker.observe(response(0xCB, 0xEF), 100);
    const auto consensus = tracker.observe(response(0xCB, 0xAF), 160);
    QC_CHECK(consensus.has_value());
    QC_CHECK_EQ(consensus->capability, SpeedCapability::Three);
    // One fan frame names the band but is not corroboration (issue #46).
    QC_CHECK_EQ(consensus->capability_evidence,
                CapabilityEvidence::Uncorroborated);
  }
}

// Issue #46: the rank is a property of the frame's SHAPE, not of the window it
// was heard in. No window can rule out the paired OEM remote, whose commands
// carry our sender ID and marker bits 10; only bit 6 — which no command
// constructor sets — makes a capability unambiguous.
QC_TEST("consensus", "only a capability no command can carry is unambiguous") {
  const std::array<std::uint8_t, 4> command_shaped{{0x9F, 0xAF, 0xBF, 0x90}};
  for (const auto byte : command_shaped) {
    ConsensusTracker tracker;
    tracker.observe(response(0xCB, byte), 100);
    const auto consensus = tracker.observe(response(0xCB, byte), 160);
    QC_CHECK(consensus.has_value());
    QC_CHECK_EQ(consensus->capability, SpeedCapability::Two);
    QC_CHECK_EQ(consensus->capability_evidence,
                CapabilityEvidence::CommandShaped);
  }
  const std::array<std::uint8_t, 2> fan_only{{0x5F, 0xDF}};
  for (const auto byte : fan_only) {
    ConsensusTracker tracker;
    tracker.observe(response(0xCB, byte), 100);
    const auto consensus = tracker.observe(response(0xCB, byte), 160);
    QC_CHECK(consensus.has_value());
    QC_CHECK_EQ(consensus->capability_evidence,
                CapabilityEvidence::Unambiguous);
  }
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

namespace quietcool {
namespace {

// Issue #46 follow-up: matching state bytes are the protocol's only integrity
// check, and the reporter's link was logged corrupting both alike. A reply
// with bit 6 lost (0xDF -> 0x9F) or bit 7 lost (0xDF -> 0x5F) is a valid frame
// with the same canonical byte and a different capability. One such frame must
// not be able to name the band of a consensus it merely completes.
QC_TEST("consensus", "one corrupted reply cannot carry a corroborated capability") {
  struct Case { std::array<std::uint8_t, 3> bytes; std::size_t size;
                SpeedCapability capability; CapabilityEvidence evidence; };
  const std::array<Case, 7> cases{{
      // Clean replies: corroborated.
      {{{0xDF, 0xDF, 0}}, 2, SpeedCapability::Three, CapabilityEvidence::Unambiguous},
      {{{0x5F, 0x5F, 0}}, 2, SpeedCapability::One, CapabilityEvidence::Unambiguous},
      // Bit 6 lost on either frame: the surviving Three stands, uncorroborated.
      {{{0xDF, 0x9F, 0}}, 2, SpeedCapability::Three, CapabilityEvidence::Uncorroborated},
      {{{0x9F, 0xDF, 0}}, 2, SpeedCapability::Three, CapabilityEvidence::Uncorroborated},
      // Bit 7 lost on either frame: one vote each, so no capability at all.
      {{{0xDF, 0x5F, 0}}, 2, SpeedCapability::Unknown, CapabilityEvidence::Uncorroborated},
      {{{0x5F, 0xDF, 0}}, 2, SpeedCapability::Unknown, CapabilityEvidence::Uncorroborated},
      // No capability metadata and one fan frame (a remote press, then a reply).
      {{{0x1F, 0xDF, 0}}, 2, SpeedCapability::Three, CapabilityEvidence::Uncorroborated},
  }};
  for (const auto& c : cases) {
    ConsensusTracker tracker;
    std::optional<Consensus> consensus;
    for (std::size_t i = 0; i < c.size; ++i)
      consensus = tracker.observe(response(0xCB, c.bytes[i]),
                                  100 + 60 * static_cast<MonotonicMs>(i));
    QC_CHECK(consensus.has_value());
    QC_CHECK_EQ(consensus->capability, c.capability);
    QC_CHECK_EQ(consensus->capability_evidence, c.evidence);
  }
}

// A same-burst repeat is not a second witness: it neither counts toward the
// consensus nor casts a capability vote.
QC_TEST("consensus", "same-burst repeats do not corroborate a capability") {
  ConsensusTracker tracker;
  tracker.observe(response(0xCB, 0x1F), 100);
  QC_CHECK(!tracker.observe(response(0xCB, 0x5F), 110).has_value());
  QC_CHECK(!tracker.observe(response(0xCB, 0x5F), 120).has_value());
  const auto consensus = tracker.observe(response(0xCB, 0x1F), 180);
  QC_CHECK(consensus.has_value());
  QC_CHECK_EQ(consensus->capability, SpeedCapability::Unknown);
}

}  // namespace
}  // namespace quietcool
