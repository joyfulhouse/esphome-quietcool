#include "quietcool/ports/authority_publication_gate.h"
#include "support/test.h"

#include <optional>

namespace quietcool {
namespace {

AuthoritySnapshot unknown_authority() {
  return {
      UnknownStateAuthority{AuthorityLossReason::Boot, 0, std::nullopt,
                            std::nullopt},
      UnknownTimerAuthority{TimerLossReason::Unknown, 0},
      std::nullopt,
      std::nullopt,
      0,
  };
}

AuthoritySnapshot confirmed_authority(std::uint64_t revision, Speed speed) {
  const auto state = FanState::command(speed, Duration::Continuous);
  return {
      ConfirmedStateAuthority{
          state,
          EvidenceSource::ManualQueryConsensus,
          EvidenceConfidence::ExactBackedConsensus,
          100,
          2,
          std::nullopt,
          std::nullopt,
          revision,
      },
      NoActiveTimerAuthority{
          false, EvidenceSource::ManualQueryConsensus, 100},
      speed,
      state,
      revision,
  };
}

QC_TEST("INV-25", "publication gate accepts confirmed authority once per revision") {
  AuthorityPublicationGate gate;
  QC_CHECK(!gate.next(unknown_authority()).has_value());

  const auto first = gate.next(confirmed_authority(1, Speed::Low));
  QC_CHECK(first.has_value());
  QC_CHECK_EQ(first->revision, 1U);
  QC_CHECK(!gate.next(confirmed_authority(1, Speed::High)).has_value());

  const auto second = gate.next(confirmed_authority(2, Speed::High));
  QC_CHECK(second.has_value());
  QC_CHECK_EQ(second->state.speed().value(), Speed::High);
}

QC_TEST("INV-25", "publication gate rejects regressed and non-confirmed revisions") {
  AuthorityPublicationGate gate;
  QC_CHECK(gate.next(confirmed_authority(7, Speed::Medium)).has_value());
  QC_CHECK(!gate.next(confirmed_authority(6, Speed::High)).has_value());
  auto unknown = unknown_authority();
  unknown.revision = 8;
  QC_CHECK(!gate.next(unknown).has_value());
  QC_CHECK(gate.next(confirmed_authority(8, Speed::Low)).has_value());
}

}  // namespace
}  // namespace quietcool
