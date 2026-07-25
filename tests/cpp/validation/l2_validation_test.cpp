#include "quietcool/core/confirmation_core.h"
#include "support/core_test_builder.h"
#include "support/test.h"

#include <cstddef>

// Issue #11 (L2), core-level guards: an ill-formed command must be refused at
// ingest without beginning a transaction, and an encode failure at lease time
// must not wedge the coordinator (state/context stay coherent; it escapes).

namespace quietcool {
namespace {

std::size_t tx_burst_count(const CoreEffects& effects) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < effects.size(); ++index)
    count += std::get_if<RequestTxBurst>(&effects[index]) != nullptr;
  return count;
}

bool has_refusal(const CoreEffects& effects) {
  for (std::size_t index = 0; index < effects.size(); ++index)
    if (std::get_if<RefusedInput>(&effects[index]) != nullptr)
      return true;
  return false;
}

QC_TEST("validation", "invalid command is refused at ingest, not wedged") {
  auto core =
      ConfirmationCoreTestBuilder::make(CoordinatorState::Idle, IdleContext{})
          .value();
  const auto effects = core.request_state(
      FanState::command(Speed::Low, static_cast<Duration>(7)), 0);
  // Refused before any transaction begins: no transmission, no state change.
  QC_CHECK(has_refusal(effects));
  QC_CHECK_EQ(tx_burst_count(effects), std::size_t{0});
  const auto snapshot = core.snapshot(0);
  QC_CHECK_EQ(snapshot.state, CoordinatorState::Idle);
  QC_CHECK(!snapshot.transaction.has_value());
}

QC_TEST("validation", "encode failure does not wedge the command lease") {
  auto core = ConfirmationCoreTestBuilder::make_unencodable_command_pending();
  QC_CHECK_EQ(core.snapshot(0).state, CoordinatorState::CommandPending);

  const auto effects = core.poll(0);
  const auto snapshot = core.snapshot(0);
  // The wedge was state_ == CommandLeaseIssued with a CommandPendingContext and
  // no live_tx_. Assert the state/context invariant holds and the machine
  // escaped to a coherent terminal instead.
  QC_CHECK(ConfirmationCore::context_matches_state(snapshot.state,
                                                   snapshot.context));
  QC_CHECK_EQ(snapshot.state, CoordinatorState::Idle);
  QC_CHECK(!snapshot.transaction.has_value());
  QC_CHECK(!snapshot.live_tx.has_value());
  QC_CHECK_EQ(tx_burst_count(effects), std::size_t{0});
  QC_CHECK(has_refusal(effects));

  // Stable: a further poll does not resurrect a lease or re-wedge.
  core.poll(1);
  QC_CHECK_EQ(core.snapshot(1).state, CoordinatorState::Idle);
}

}  // namespace
}  // namespace quietcool
