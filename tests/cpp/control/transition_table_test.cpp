#include "quietcool/core/confirmation_core.h"
#include "quietcool/core/fan_state.h"
#include "quietcool/core/transition_table.h"
#include "support/test.h"

#include <array>
#include <cstdint>
#include <set>
#include <vector>

namespace quietcool {
namespace {

bool computed_transition_is_justified(const TransitionRule& rule) {
  switch (rule.action) {
    case ActionId::TrackCandidate:
    case ActionId::HandleRestore:
    case ActionId::HandleRadioReady:
    case ActionId::HandleCommandRequest:
    case ActionId::HandleTimerExpiry:
    case ActionId::HandleHoldoffExpired:
    case ActionId::HandleRecoveryDue:
    case ActionId::HandleLearningFrame:
    case ActionId::HandleLearnExpiry:
    case ActionId::HandleRadioRecovered:
    case ActionId::HandleHeartbeatRequest:
      return true;
    case ActionId::HandleLearnRequest:
      // Owns its resulting state (issue #16): LearningAwaitingFirst when the
      // core is unprovisioned, unchanged on the AlreadyProvisioned refusal. A
      // fixed next-state would overwrite the refusal into a learn window.
      return true;
    case ActionId::IssueCommandLease:
      // Owns its resulting state: CommandLeaseIssued on success, Idle on an
      // encode-failure refusal (issue #11 wedge fix).
      return rule.state == CoordinatorState::CommandPending &&
             rule.event == EventKind::Poll;
    case ActionId::FinishQueryMiss:
      return rule.state == CoordinatorState::RecoveryResponseListening;
    case ActionId::HandleTailFrame:
      return rule.state == CoordinatorState::ResponseTailQuarantine &&
             rule.event == EventKind::TailExpired;
    case ActionId::BeginRadioRecovery:
      return rule.state == CoordinatorState::RadioRecovery;
    default:
      return false;
  }
}

QC_TEST("transition_table", "transaction consensus matrix has exactly one match") {
  const std::array<Duration, 7> durations{
      Duration::Off, Duration::Hours1, Duration::Hours2, Duration::Hours4,
      Duration::Hours8, Duration::Hours12, Duration::Continuous};
  std::uint64_t generated = 0;
  for (const auto speed : {Speed::Low, Speed::Medium, Speed::High}) {
    for (const auto duration : durations) {
      const auto requested = FanState::command(speed, duration);
      for (std::uint16_t raw = 0; raw <= 0xFF; ++raw) {
        const auto candidate = FanState::observed(static_cast<std::uint8_t>(raw));
        if (!candidate) continue;
        for (const auto prior : {PriorRelation::Absent, PriorRelation::Equal,
                                 PriorRelation::Unequal}) {
          for (const bool attempts : {false, true}) {
            const TransactionConsensusInput input{
                requested.semantically_equals(candidate.value()),
                requested.is_on(), candidate.value().is_on(),
                candidate.value().has_outbound_command_marker(), prior,
                attempts};
            const auto post = ConfirmationCore::matching_transaction_rules_for_test(
                CoordinatorState::PostCommandListening, input);
            const auto fallback = ConfirmationCore::matching_transaction_rules_for_test(
                CoordinatorState::FallbackResponseListening, input);
            QC_CHECK_EQ(post.count, 1U);
            QC_CHECK_EQ(fallback.count, 1U);
            QC_CHECK_EQ(post.first.guard, fallback.first.guard);
            QC_CHECK_EQ(post.first.action, fallback.first.action);
            ++generated;
          }
        }
      }
    }
  }
  QC_CHECK(generated > 10000);
}

QC_TEST("transition_table", "ON mismatches with attempts never fall through") {
  for (std::uint16_t raw = 0; raw <= 0xFF; ++raw) {
    const auto candidate = FanState::observed(static_cast<std::uint8_t>(raw));
    if (!candidate) continue;
    const auto requested = FanState::command(Speed::Low, Duration::Continuous);
    if (requested.semantically_equals(candidate.value())) continue;
    const TransactionConsensusInput input{
        false, true, candidate.value().is_on(),
        candidate.value().has_outbound_command_marker(), PriorRelation::Absent,
        true};
    const auto match = ConfirmationCore::matching_transaction_rules_for_test(
        CoordinatorState::PostCommandListening, input);
    QC_CHECK_EQ(match.count, 1U);
    QC_CHECK(match.first.action != ActionId::InvalidInternalEvent);
  }
}

QC_TEST("transition_table", "rule IDs are unique and priorities monotonic") {
  const auto view = TransitionTable::rules();
  std::set<std::uint16_t> ids;
  for (std::size_t index = 0; index < view.size; ++index) {
    QC_CHECK(ids.insert(view.data[index].rule_id).second);
    if (index == 0) continue;
    const auto& before = view.data[index - 1];
    const auto& rule = view.data[index];
    if (before.state == rule.state && before.event == rule.event)
      QC_CHECK(before.priority < rule.priority);
  }
}

QC_TEST("transition_table", "every non-Computed next state resolves authoritatively") {
  const auto view = TransitionTable::rules();
  std::size_t computed_rows = 0;
  std::size_t heartbeat_rows = 0;
  for (std::size_t index = 0; index < view.size; ++index) {
    const auto& rule = view.data[index];
    if (rule.action == ActionId::HandleHeartbeatRequest) ++heartbeat_rows;
    const auto resolved =
        TransitionTable::fixed_next_state(rule.next, rule.state);
    if (rule.next == NextStateId::Computed) {
      QC_CHECK(!resolved.has_value());
      QC_CHECK(computed_transition_is_justified(rule));
      ++computed_rows;
      continue;
    }
    QC_CHECK(resolved.has_value());
    if (rule.next == NextStateId::Same)
      QC_CHECK_EQ(*resolved, rule.state);
  }
  QC_CHECK(computed_rows > 0U);
  QC_CHECK_EQ(heartbeat_rows, kCoordinatorStateCount);
}

QC_TEST("transition_table", "query families share template but keep state IDs") {
  for (const auto origin : {TemplateOrigin::BootQuery, TemplateOrigin::ManualQuery,
                            TemplateOrigin::FallbackQuery,
                            TemplateOrigin::RecoveryQuery}) {
    QC_CHECK_EQ(TransitionTable::query_template_rule_count(origin), 10U);
  }

  const auto view = TransitionTable::rules();
  std::array<std::vector<const TransitionRule*>, 4> families;
  for (std::size_t index = 0; index < view.size; ++index) {
    const auto origin = view.data[index].origin;
    if (origin >= TemplateOrigin::BootQuery &&
        origin <= TemplateOrigin::RecoveryQuery)
      families[static_cast<std::size_t>(origin) -
               static_cast<std::size_t>(TemplateOrigin::BootQuery)]
          .push_back(&view.data[index]);
  }
  for (std::size_t family = 1; family < families.size(); ++family) {
    QC_CHECK_EQ(families[family].size(), families[0].size());
    for (std::size_t row = 0; row < families[0].size(); ++row) {
      QC_CHECK_EQ(families[family][row]->event, families[0][row]->event);
      QC_CHECK_EQ(families[family][row]->guard, families[0][row]->guard);
      QC_CHECK_EQ(families[family][row]->action, families[0][row]->action);
      QC_CHECK_EQ(families[family][row]->priority,
                  families[0][row]->priority);
    }
  }
}

QC_TEST("transition_table", "all states have RF permission and Refresh policy") {
  for (std::uint8_t value = 0; value < kCoordinatorStateCount; ++value) {
    const auto state = static_cast<CoordinatorState>(value);
    const auto permission = TransitionTable::rf_permission(state);
    if (state == CoordinatorState::CommandPending) QC_CHECK(permission.lease_command);
    if (state == CoordinatorState::BootQueryPending ||
        state == CoordinatorState::ManualQueryPending ||
        state == CoordinatorState::FallbackQueryPending ||
        state == CoordinatorState::RecoveryQueryPending)
      QC_CHECK(permission.lease_query);
    QC_CHECK_EQ(TransitionTable::refresh_is_accepted(state),
                state == CoordinatorState::Idle);
  }
}

QC_TEST("transition_table", "fallback has one contractual predecessor") {
  const auto view = TransitionTable::rules();
  std::size_t predecessors = 0;
  for (std::size_t index = 0; index < view.size; ++index) {
    const auto& rule = view.data[index];
    if (rule.next == NextStateId::FallbackQueryPending) {
      ++predecessors;
      QC_CHECK_EQ(rule.state, CoordinatorState::PostCommandTailWait);
      QC_CHECK_EQ(rule.event, EventKind::TailExpired);
    }
    QC_CHECK(!(rule.state == CoordinatorState::ResponseTailQuarantine &&
               rule.next == NextStateId::FallbackQueryPending));
  }
  QC_CHECK_EQ(predecessors, 1U);
}

// Deliberately no "every action is dispatchable" test here: the real dispatcher
// (ConfirmationCore::dispatch) has no `default:` label, so -Wswitch -Werror
// already forces every ActionId to be handled at compile time, and each action's
// actual behaviour is pinned by the INV/REG/supersession/tail-exit suites. A
// hand-maintained surrogate switch that returns true for every ActionId proved
// none of that — it only restated the enum — so it was removed (issue #12, L3-3).

QC_TEST("transition_table", "every state-event group is first-match ordered") {
  constexpr std::size_t state_count = kCoordinatorStateCount;
  constexpr std::size_t event_count =
      static_cast<std::size_t>(EventKind::Poll) + 1U;
  std::array<std::uint16_t, state_count * event_count> priorities{};
  const auto view = TransitionTable::rules();
  for (std::size_t index = 0; index < view.size; ++index) {
    const auto& rule = view.data[index];
    const auto key = static_cast<std::size_t>(rule.state) * event_count +
                     static_cast<std::size_t>(rule.event);
    QC_CHECK_EQ(rule.priority, priorities[key] + 1U);
    priorities[key] = rule.priority;
  }
}

QC_TEST("transition_table", "global reducer events cover every state") {
  const std::array<EventKind, 8> global_events{
      EventKind::ProvisioningRestored, EventKind::RadioReady,
      EventKind::CommandRequested, EventKind::ManualRefreshRequested,
      EventKind::LearnRequested, EventKind::ForgetRequested,
      EventKind::ExactOemQueryHeard, EventKind::TimerEstimateExpired};
  const auto view = TransitionTable::rules();
  for (const auto event : global_events) {
    std::array<bool, kCoordinatorStateCount> covered{};
    for (std::size_t index = 0; index < view.size; ++index)
      if (view.data[index].event == event)
        covered[static_cast<std::size_t>(view.data[index].state)] = true;
    for (const bool state_is_covered : covered) QC_CHECK(state_is_covered);
  }
}

QC_TEST("transition_table", "recovery retry tail has a typed first-match row") {
  const auto view = TransitionTable::rules();
  std::size_t matches = 0;
  for (std::size_t index = 0; index < view.size; ++index) {
    const auto& rule = view.data[index];
    if (rule.state == CoordinatorState::ResponseTailQuarantine &&
        rule.event == EventKind::TailExpired &&
        rule.guard == GuardId::TailBeginsRecoveryRetry) {
      QC_CHECK_EQ(rule.next, NextStateId::RecoveryRetryWait);
      ++matches;
    }
  }
  QC_CHECK_EQ(matches, 1U);
}

}  // namespace
}  // namespace quietcool
