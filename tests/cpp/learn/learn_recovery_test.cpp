#include "quietcool/core/learn_machine.h"
#include "quietcool/core/recovery_scheduler.h"
#include "support/test.h"

#include <array>

namespace quietcool {
namespace {

constexpr std::array<std::uint8_t, 6> command(std::uint8_t id,
                                              std::uint8_t state) {
  return {0xCB, 0x00, 0x47, id, state, state};
}

QC_TEST("learn", "sighting gap and window span boundaries") {
  LearnMachine learn;
  learn.start(LearnMode::Manual, 0);
  const auto first = command(0x39, 0x9F);
  QC_CHECK_EQ(learn.observe(ByteView(first), 0).kind,
              LearnEventKind::CandidateStarted);
  // Frames closer than kLearnSightingGapMs are one burst -> not a new sighting.
  QC_CHECK_EQ(learn.observe(ByteView(first), 599).kind, LearnEventKind::Ignored);
  // A gap >= kLearnSightingGapMs is an independent sighting, but two of them
  // (start + this one) is still short of the three-sighting bar.
  QC_CHECK_EQ(learn.observe(ByteView(first), 600).kind, LearnEventKind::Ignored);
  // Third independent sighting binds.
  QC_CHECK_EQ(learn.observe(ByteView(first), 1200).kind,
              LearnEventKind::Learned);

  // A same-sender frame older than the window span restarts the candidate
  // rather than counting as a late sighting.
  learn.start(LearnMode::Manual, 0);
  learn.observe(ByteView(first), 0);
  QC_CHECK_EQ(learn.observe(ByteView(first), 59999).kind,
              LearnEventKind::Ignored);
  learn.start(LearnMode::Manual, 0);
  learn.observe(ByteView(first), 0);
  QC_CHECK_EQ(learn.observe(ByteView(first), 60000).kind,
              LearnEventKind::CandidateRestarted);
}

QC_TEST("learn", "two competing senders refuse and persist nothing") {
  LearnMachine learn;
  learn.start(LearnMode::Manual, 0);
  QC_CHECK_EQ(learn.observe(ByteView(command(0x39, 0x9F)), 0).kind,
              LearnEventKind::CandidateStarted);
  QC_CHECK_EQ(learn.observe(ByteView(command(0x40, 0xAF)), 700).kind,
              LearnEventKind::AmbiguousRejected);
  QC_CHECK_EQ(learn.observe(ByteView(command(0x40, 0xBF)), 1400).kind,
              LearnEventKind::Ignored);
  QC_CHECK(!learn.snapshot().candidate.has_value());
  QC_CHECK(!learn.snapshot().active);
}

QC_TEST("learn", "exclusive sender needs three independent sightings") {
  LearnMachine learn;
  learn.start(LearnMode::Manual, 0);
  const auto first = command(0x39, 0x9F);
  QC_CHECK_EQ(learn.observe(ByteView(first), 0).kind,
              LearnEventKind::CandidateStarted);
  QC_CHECK_EQ(learn.observe(ByteView(first), 601).kind,
              LearnEventKind::Ignored);
  QC_CHECK_EQ(learn.observe(ByteView(first), 1202).kind,
              LearnEventKind::Learned);
}

QC_TEST("learn", "a single burst is one sighting") {
  LearnMachine learn;
  learn.start(LearnMode::Manual, 0);
  const auto first = command(0x39, 0x9F);
  QC_CHECK_EQ(learn.observe(ByteView(first), 0).kind,
              LearnEventKind::CandidateStarted);  // sighting 1
  QC_CHECK_EQ(learn.observe(ByteView(first), 700).kind,
              LearnEventKind::Ignored);  // sighting 2
  // 760 and 820 are within kLearnSightingGapMs of the last counted sighting
  // (700): a single OEM self-report burst, so they must not each count.
  QC_CHECK_EQ(learn.observe(ByteView(first), 760).kind,
              LearnEventKind::Ignored);
  QC_CHECK_EQ(learn.observe(ByteView(first), 820).kind,
              LearnEventKind::Ignored);
  const auto snapshot = learn.snapshot();
  QC_CHECK(snapshot.candidate.has_value());
  QC_CHECK_EQ(snapshot.candidate->sightings, std::uint8_t{2});
}

QC_TEST("learn", "ambiguity ignores the remainder of the window") {
  LearnMachine learn;
  learn.start(LearnMode::Manual, 0);
  QC_CHECK_EQ(learn.observe(ByteView(command(0x39, 0x9F)), 0).kind,
              LearnEventKind::CandidateStarted);
  QC_CHECK_EQ(learn.observe(ByteView(command(0x40, 0xAF)), 700).kind,
              LearnEventKind::AmbiguousRejected);
  QC_CHECK(!learn.snapshot().active);
  // Even the original first sender cannot resume a poisoned window.
  QC_CHECK_EQ(learn.observe(ByteView(command(0x39, 0x9F)), 1400).kind,
              LearnEventKind::Ignored);
  QC_CHECK_EQ(learn.observe(ByteView(command(0x39, 0x9F)), 2100).kind,
              LearnEventKind::Ignored);
}

QC_TEST("learn", "queries reports malformed and special frames never participate") {
  LearnMachine learn;
  learn.start(LearnMode::Manual, 0);
  for (const auto frame : {command(0x39, 0x66), command(0x39, 0x5F),
                           command(0x39, 0x13)}) {
    QC_CHECK_EQ(learn.observe(ByteView(frame), 10).kind, LearnEventKind::Ignored);
  }
  const std::array<std::uint8_t, 6> special{0xCE, 0x00, 0x47, 0x39, 0x9F, 0x9F};
  QC_CHECK_EQ(learn.observe(ByteView(special), 10).kind, LearnEventKind::Ignored);
  QC_CHECK(!learn.snapshot().candidate.has_value());
}

QC_TEST("learn", "hard deadlines expire and learning is receive-only") {
  LearnMachine learn;
  learn.start(LearnMode::Manual, 100);
  QC_CHECK_EQ(learn.poll(120099).kind, LearnEventKind::Ignored);
  QC_CHECK_EQ(learn.poll(120100).kind, LearnEventKind::WindowExpired);
  learn.start(LearnMode::Automatic, 0);
  QC_CHECK_EQ(learn.snapshot().deadline_ms, 900000U);
}

QC_TEST("recovery_scheduler", "OEM recovery has one initial and one retry") {
  RecoveryScheduler recovery(17);
  recovery.arm_from_oem_activity(0);
  QC_CHECK_EQ(recovery.poll(2999).status, RecoveryDueStatus::NotDue);
  QC_CHECK_EQ(recovery.poll(3000).status, RecoveryDueStatus::QueryDue);
  recovery.note_query_started(3000);
  recovery.note_empty_window(3000);
  const auto snapshot = recovery.snapshot();
  QC_CHECK(snapshot.due_ms >= 6000 && snapshot.due_ms <= 6500);
  QC_CHECK_EQ(recovery.poll(snapshot.due_ms).status, RecoveryDueStatus::QueryDue);
  recovery.note_query_started(snapshot.due_ms);
  recovery.note_empty_window(snapshot.due_ms);
  QC_CHECK_EQ(recovery.snapshot().phase, RecoveryPhase::Complete);
}

QC_TEST("recovery_scheduler", "repeated OEM evidence restarts quiet without budget renewal") {
  RecoveryScheduler recovery(3);
  recovery.arm_from_oem_activity(0);
  for (MonotonicMs now = 1000; now <= 11000; now += 1000) {
    recovery.arm_from_oem_activity(now);
    QC_CHECK_EQ(recovery.poll(now).status, RecoveryDueStatus::NotDue);
  }
  QC_CHECK_EQ(recovery.poll(13999).status, RecoveryDueStatus::NotDue);
  QC_CHECK_EQ(recovery.poll(14000).status, RecoveryDueStatus::QueryDue);
}

QC_TEST("recovery_scheduler", "timer expiry is one shot and expires after 5000 ms") {
  RecoveryScheduler recovery(9);
  recovery.arm_from_timer_expiry(1000);
  const auto due = recovery.snapshot().due_ms;
  QC_CHECK(due >= 1500 && due <= 2000);
  QC_CHECK_EQ(recovery.poll(due).status, RecoveryDueStatus::QueryDue);
  recovery.note_query_started(due);
  recovery.note_empty_window(due);
  QC_CHECK_EQ(recovery.snapshot().phase, RecoveryPhase::Complete);
  recovery.arm_from_timer_expiry(10000);
  QC_CHECK_EQ(recovery.poll(15001).status, RecoveryDueStatus::Expired);
}

}  // namespace
}  // namespace quietcool
