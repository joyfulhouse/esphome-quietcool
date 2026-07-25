#pragma once

#include "quietcool/core/confirmation_core.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace quietcool {

class ConfirmationCoreTestBuilder final {
 public:
  static std::optional<ConfirmationCore> make(CoordinatorState state,
                                               StateContext context) {
    if (!ConfirmationCore::context_matches_state(state, context))
      return std::nullopt;
    ConfirmationCore core(CoreConfig{0});
    RestorableState restored;
    restored.sender = SenderId::from_be_u32(0xCB004739U).value();
    core.handle_restore(restored, 0);
    core.state_ = state;
    core.context_ = std::move(context);
    if (state == CoordinatorState::RetryDelay) {
      const auto id = core.transaction_ids_.allocate();
      core.transaction_ = CommandTransaction::begin(
          *id, FanState::command(Speed::Low, Duration::Continuous),
          std::nullopt);
      core.authority_.begin_local_command(*id, 0);
    }
    return core;
  }

  // Seats CommandPending with a live transaction whose outbound cannot be
  // encoded (an OFF command re-aimed to a cast-invalid speed). A poll from here
  // fires IssueCommandLease and reaches issue_command's encode-failure branch —
  // the issue #11 wedge trigger that ingest validation can no longer reach.
  static ConfirmationCore make_unencodable_command_pending() {
    ConfirmationCore core(CoreConfig{0});
    RestorableState restored;
    restored.sender = SenderId::from_be_u32(0xCB004739U).value();
    core.handle_restore(restored, 0);
    const auto id = core.transaction_ids_.allocate();
    core.transaction_ = CommandTransaction::begin(
        *id, FanState::command(Speed::Low, Duration::Off), std::nullopt);
    core.transaction_->reaim_off_to(static_cast<Speed>(4));
    core.authority_.begin_local_command(*id, 0);
    core.state_ = CoordinatorState::CommandPending;
    core.context_ = CommandPendingContext{0};
    return core;
  }

  static void set_next_transaction_id(ConfirmationCore& core,
                                      std::uint64_t next) {
    core.transaction_ids_ = MonotonicIdAllocator<TransactionId>(next);
  }
  static void set_next_tx_token(ConfirmationCore& core, std::uint64_t next) {
    core.tx_tokens_ = MonotonicIdAllocator<TxToken>(next);
  }
};

}  // namespace quietcool
