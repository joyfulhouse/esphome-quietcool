# QuietCool C++ confirmation core design

Status: design contract only
Date: 2026-07-21
Target worktree: <code>/Users/bryanli/Projects/joyfulhouse/esphome-quietcool-cpp</code>
Target branch baseline: <code>feat/cpp-core</code> at <code>c676bbf</code>
Implementation authorization: none

## 1. Purpose and decision

This document specifies a platform-neutral C++ confirmation core and the thin
ESPHome integration around it.
It is intentionally a design, not a partial implementation.
The implementation phase must be reviewed against this contract.
The central design decision is:

> Passive post-command listening, active local-query listening, external OEM
> priority, learning, and tail quarantine are distinct typed states. They can
> never share a boolean or be inferred from a combination of unrelated flags.

The coordinator is an explicit 31-state machine.
All state transitions occur in one table-driven reducer owned by
<code>ConfirmationCore</code>.
Passive transition descriptors may live in a separate table module, but that
module cannot inspect or mutate coordinator state.
All guards name concrete enum states.
Classification answers what a received frame could be in the current timing
context.
Acceptance separately answers what that evidence is allowed to change.
Only <code>AuthorityStore</code> may change public fan-state or timer authority.
The common command path is:

1. Emit one bounded three-frame state-command burst.
2. Start a passive response epoch at the exact burst-completion timestamp.
3. Accept response candidates only in the measured post-command window.
4. Confirm from consensus over the fan's unsolicited report.
5. If and only if that report misses consensus, wait out the old response tail
   and emit exactly one fallback status-query burst for that command attempt.
6. Retry the state command only after the fallback result or miss, using the
   original fixed and nonrenewable attempt budget.

No status query is sent before the fan's measured actuation point.

## 2. Evidence and non-negotiable physical facts

The evidence base is:

- <code>docs/claude/2026-07-20-confirmation-path-findings.md</code>, including
  its 2026-07-21 postscript.
- The Rust reference at
  <code>/Users/bryanli/Projects/joyfulhouse/homeassistant-dev/quietcool-rust/crates/quietcool-core</code>.
- The current YAML only as migration and compatibility evidence, not as a
  state-machine template.

The following are measured physical facts and are not to be re-derived in the
implementation:

- The fan emits an unsolicited three-frame state report after every command it
  receives.
- Across seven API-driven trials, the report mean was 1203 ms after the TX
  request.
- The observed report range was 1154 through 1257 ms after the TX request.
- From the end of the command burst, the report appeared at +705 through
  +807 ms.
- The report also occurs for a no-op command.
- The fan actuates at approximately 1.2 seconds.
- A status query before actuation returns the pre-command state.
- A fan reply and an OEM remote command from the same sender are byte-identical.
- Timing is the only available direction discriminator.
- The ambiguity is physical and cannot be removed in software.
- An application payload is exactly six bytes.
- Bytes 0 through 3 are the sender ID in wire order.
- Bytes 4 and 5 are the same command byte repeated.
- <code>0x66 0x66</code> is the wake/status query.
- In a state byte, speed is bits 5:4 and duration is bits 3:0.
- The radio profile is 433.920 MHz FSK, 2.4 kbps, plus or minus 10 kHz
  deviation, half-duplex.

The implementation must preserve an explicit citation beside each timing
constant.
Changing a timing constant requires:

- a named hardware capture,
- a design-note update,
- host boundary-test updates,
- and an on-hardware validation run.

It must never be changed solely to make a unit test pass.

## 3. Scope

### 3.1 In scope

- Domain validation for sender IDs, speeds, durations, and fan states.
- Six-byte protocol encoding and strict decoding.
- Tightly bounded recovery of known RX callback corruption.
- Response consensus.
- Two-press sender learning.
- Explicit command-confirmation transactions.
- Passive post-command response correlation.
- One fallback query per missed post-command response epoch.
- Fixed command re-fire budgets.
- External OEM priority and bounded authority recovery.
- Separate fan-state and timer authority.
- Host-testable monotonic timing.
- Persistence messages without any storage-framework dependency.
- A single radio interface and two thin radio adapters.
- A thin ESPHome component and entity bridge.
- A staged migration from both existing YAML configurations.

### 3.2 Out of scope for the core

- OLED layout and animation.
- Battery, temperature, Wi-Fi, and API sensors.
- ESPHome entity rendering details.
- NVS or ESPHome preferences calls.
- Arduino, ESP-IDF, or ESPHome logging.
- SPI, GPIO, and radio-register configuration.
- OTA and upload behavior.
- Re-proving physical timing on a host.
- Guessing fan state from an uncorrelated RF frame.
- Publishing estimated timer expiry as confirmed OFF.

### 3.3 Success criteria

The design succeeds only if:

- no core header includes an ESPHome, Arduino, or ESP-IDF header;
- a macOS host test can instantiate every core class;
- passive post-command listening and active query listening are different
  enum states;
- every automatic RF effect has a finite typed budget;
- exact OEM query priority is global and testable;
- manual Refresh cannot transmit in any post-command state;
- fan-state authority and timer authority cannot be represented by the same
  flag;
- <code>control.cpp</code> cannot accumulate protocol, consensus, learning,
  authority, or timer-recovery algorithms;
- SX127x and SX126x share all core behavior and burst timing;
- and the two YAML files become configuration and wiring rather than copies of
  the confirmation machine.

## 4. Design principles

### 4.1 Make invalid combinations unrepresentable

There is no equivalent of:

- <code>cl_active</code>,
- <code>cl_query_window</code>,
- <code>cl_query_epoch</code>,
- <code>cl_query_epoch_confirmation</code>,
- <code>cl_query_response_complete</code>,
- <code>cl_report_ready</code>,
- <code>fan_state_known</code>,
- <code>timer_state_known</code>,
- or a sentinel byte such as <code>0xFF</code> meaning several different
  things.

An operation has exactly one <code>CoordinatorState</code>.
Authority has exactly one <code>StateAuthority</code> variant and exactly one
<code>TimerAuthority</code> variant.
A response epoch has exactly one <code>ResponseEpoch</code> variant.
A pending tail has exactly one <code>TailExit</code> variant.
A transmit lease has exactly one <code>TxProgress</code> variant.

### 4.2 One transition owner

Only <code>ConfirmationCore</code> changes <code>CoordinatorState</code>.
Collaborators return typed facts or decisions.
They do not call each other.
They do not send RF.
They do not publish ESPHome entities.
They do not mutate coordinator state through shared references.
The transition table is passive data: it names state, event, guard, action,
next state, and precedence IDs.
Only the reducer evaluates those IDs and applies the selected action.

### 4.3 Effects are explicit

The reducer returns a small fixed-capacity batch of effects.
Representative effects are:

- request one logical RF burst;
- revoke an unstarted transmit lease;
- publish one typed core event;
- request persistence;
- publish an authority snapshot;
- request a radio reset;
- and report that an input was refused.

An effect describes intent.
The adapter performs it and reports completion with the matching token.

### 4.4 No hidden clock

The core never calls <code>millis()</code>.
Every time-sensitive public core entry point receives
<code>MonotonicMs now_ms</code>.
The adapter owns an injected <code>Clock</code>.
On ESP32, the clock adapter extends the platform's wrapping counter into a
64-bit monotonic value.
Host tests inject <code>FakeClock</code>.

### 4.5 No implicit retry

Only these objects own retry counters:

- <code>CommandTransaction</code> owns command-attempt budget.
- <code>RecoveryScheduler</code> owns the bounded OEM recovery-query budget.
- <code>RadioRecoveryContext</code> owns a bounded driver-reset budget.

No loop, interval, callback, or radio adapter may create another retry path.

## 5. Proposed source tree

The implementation phase should create the following structure.
The paths are contractual, but no files in this list are created in this
design phase.

    components/quietcool/
      __init__.py
      fan.py
      button.py
      sensor.py
      text_sensor.py
      core/
        core_types.h
        sender_id.h
        sender_id.cpp
        fan_state.h
        fan_state.cpp
        frame_codec.h
        frame_codec.cpp
        frame_recovery.h
        frame_recovery.cpp
        consensus_tracker.h
        consensus_tracker.cpp
        response_window.h
        response_window.cpp
        response_classifier.h
        response_classifier.cpp
        command_transaction.h
        command_transaction.cpp
        authority_store.h
        authority_store.cpp
        observation_policy.h
        observation_policy.cpp
        recovery_scheduler.h
        recovery_scheduler.cpp
        learn_machine.h
        learn_machine.cpp
        transition_table.h
        transition_table.cpp
        confirmation_core.h
        confirmation_core.cpp
      ports/
        clock.h
        clock.cpp
        radio.h
        radio.cpp
        event_sink.h
        event_sink.cpp
      radio/
        burst_transmitter.h
        burst_transmitter.cpp
        sx127x_radio_adapter.h
        sx127x_radio_adapter.cpp
        sx126x_radio_adapter.h
        sx126x_radio_adapter.cpp
      esphome/
        quietcool_component.h
        quietcool_component.cpp
        quietcool_fan.h
        quietcool_fan.cpp
    tests/
      cpp/
        support/
        domain/
        protocol/
        consensus/
        classification/
        authority/
        control/
        learn/
        radio/

### 5.1 File and class discipline

The following rules are hard implementation-review gates:

- One behavior-bearing production class per header/source pair.
- Pure enums and passive structs may be grouped in
  <code>core_types.h</code>.
- Pure abstract port interfaces each still receive their own pair.
- No production header over 250 lines.
- No production source over 400 lines.
- <code>confirmation_core.cpp</code> target size is 250 lines and hard maximum
  is 350 lines.
- <code>transition_table.cpp</code> has the ordinary 400-line source maximum;
  repeated query-family and transaction-consensus rows must be generated from
  shared <code>constexpr</code> row templates rather than copied.
- No test source over 400 lines.
- No method over 60 lines without a design-review exception.
- No method with more than one screen of transition cases.
- No class may own both RF parsing and coordinator transitions.
- No class may own both authority and transaction budget.
- No class may own both learning and operational control.
- No adapter may implement confirmation policy.
- No radio adapter may implement burst repetition, retry, or timing policy.

The reducer-size decision is fixed before implementation:

- <code>transition_table.cpp</code> owns only static descriptors and row-template
  expansion;
- <code>confirmation_core.cpp</code> owns first-match selection, guard/action
  dispatch, state/context validation, and all mutation;
- policy decisions stay in the already named policy collaborators;
- the table cannot contain callbacks, member pointers, or references that can
  mutate <code>ConfirmationCore</code>;
- and no helper lambda may close over coordinator state.

If either file approaches its limit, compact the passive descriptors or move
a pure decision into an already named policy collaborator. Do not create a
second transition owner and do not raise the cap without a design revision.

## 6. Dependency DAG

The dependency graph is acyclic.
Arrows mean may include or call.

    core_types
       |
       +--> sender_id
       |
       +--> fan_state
                 |
                 +--> frame_codec
                 |
                 +--> frame_recovery
                            |
                            +--> consensus_tracker

    core_types --> response_window --> response_classifier
    frame_codec ---------------------> response_classifier
    frame_recovery ------------------> response_classifier

    fan_state --> command_transaction
    fan_state --> authority_store
    fan_state --> observation_policy
    command_transaction -------------> observation_policy
    authority_store -----------------> observation_policy

    core_types --> recovery_scheduler
    core_types --> transition_table
    sender_id  --> learn_machine
    fan_state  --> learn_machine

    response_classifier --+
    consensus_tracker ----+
    command_transaction --+
    authority_store ------+--> confirmation_core
    observation_policy ---+
    recovery_scheduler ---+
    learn_machine --------+
    transition_table -----+

    clock --------+
    radio --------+--> burst_transmitter
                     |
                     +--> sx127x_radio_adapter
                     +--> sx126x_radio_adapter

    confirmation_core ----+
    burst_transmitter ----+--> quietcool_component --> ESPHome entities
    event_sink -----------+

Forbidden reverse dependencies are:

- domain to protocol;
- protocol to control;
- consensus to control;
- any core class to ports, ESPHome, Arduino, ESP-IDF, or a radio component;
- radio adapters to confirmation core internals;
- and entity classes to protocol or classifier internals.

## 7. Module contracts

All signature sketches in this section are illustrative API contracts.
They are intentionally incomplete and non-buildable.
They show names, ownership, and dependency direction, not implementation.

### 7.1 core_types

Files:

- <code>core/core_types.h</code>

Responsibility:

- Define passive enums, opaque numeric IDs, timestamps, fixed-size frame
  containers, events, effects, and snapshots shared across modules.

Owns:

- No mutable state.
- No algorithms.

May depend on:

- C++17 standard headers that do not allocate by themselves.

Must not depend on:

- Any other project module.
- Any platform header.

Illustrative declarations:

    using MonotonicMs = std::uint64_t;

    struct TransactionId;
    struct TxToken;
    struct FrameBytes;
    struct CoreEvent;
    struct CoreEffect;
    struct CoreSnapshot;

    enum class CoordinatorState : std::uint8_t;
    enum class TxReason : std::uint8_t;
    enum class TransactionOutcome : std::uint8_t;
    enum class RefusalReason : std::uint8_t;

The implementation must use opaque wrappers rather than passing bare integers
for transaction IDs, TX tokens, timestamps, attempts, or sender IDs.

### 7.2 SenderId

Files:

- <code>core/sender_id.h</code>
- <code>core/sender_id.cpp</code>

Class:

- <code>SenderId</code>

Responsibility:

- Represent one validated four-byte sender ID in wire order.

Owns:

- Four bytes.

May depend on:

- <code>core_types</code>.

Public API:

    class SenderId final {
     public:
      static Result<SenderId, SenderIdError> from_bytes(
          std::array<std::uint8_t, 4> bytes);
      static Result<SenderId, SenderIdError> from_be_u32(std::uint32_t value);
      std::array<std::uint8_t, 4> bytes() const;
      std::uint32_t as_be_u32() const;
      bool operator==(const SenderId&) const;
    };

Rules:

- Operational IDs start with <code>0xCB</code>.
- Zero is not a valid object and cannot be a default value.
- Unprovisioned state is <code>std::optional&lt;SenderId&gt;</code>, never a
  magic numeric sender.
- Formatting is adapter work, not core identity logic.

### 7.3 FanState

Files:

- <code>core/fan_state.h</code>
- <code>core/fan_state.cpp</code>

Class:

- <code>FanState</code>

Responsibility:

- Validate and expose physical command semantics of one state byte.

Owns:

- The original raw byte.
- Whether the byte came from observation or outbound construction.

May depend on:

- <code>core_types</code>.

Public API:

    enum class Speed : std::uint8_t { Low = 1, Medium = 2, High = 3 };

    enum class Duration : std::uint8_t {
      Off = 0,
      Hours1 = 1,
      Hours2 = 2,
      Hours4 = 4,
      Hours8 = 8,
      Hours12 = 12,
      Continuous = 15
    };

    enum class SpeedCapability : std::uint8_t {
      Unknown = 0,
      One = 1,
      Two = 2,
      Three = 3
    };

    class FanState final {
     public:
      static FanState command(Speed speed, Duration duration);
      static Result<FanState, StateError> observed(std::uint8_t raw);
      std::uint8_t raw_byte() const;
      std::uint8_t canonical_byte() const;
      std::uint8_t outbound_command_byte() const;
      std::optional<Speed> speed() const;
      Duration duration() const;
      std::optional<SpeedCapability> report_capability() const;
      bool is_on() const;
      bool has_outbound_command_marker() const;
      bool semantically_equals(const FanState& other) const;
    };

Rules:

- Canonical identity masks only metadata bits 7:6.
- Metadata masking never touches sender bytes.
- All OFF states are semantically equivalent for confirmation.
- Outbound OFF still requires a remembered speed.
- A running state with speed zero is invalid.
- Undefined duration nibbles are invalid.

### 7.4 FrameCodec

Files:

- <code>core/frame_codec.h</code>
- <code>core/frame_codec.cpp</code>

Class:

- <code>FrameCodec</code>

Responsibility:

- Encode exact six-byte queries and commands.
- Strictly decode untrusted six-byte frames.

Owns:

- No mutable state.

May depend on:

- <code>SenderId</code>.
- <code>FanState</code>.

Public API:

    class FrameCodec final {
     public:
      static FrameBytes encode_query(SenderId sender);
      static Result<FrameBytes, FrameEncodeError> encode_state(
          SenderId sender,
          FanState state);
      static Result<DecodedFrame, FrameDecodeError> decode_strict(
          ByteView input,
          SenderId provisioned_sender);
    };

    using DecodedFrame =
        variant<ExactQuery, ExactState, ExactSpecialResponse>;

Rules:

- Length must be exactly six for strict decode.
- Tail bytes must be equal.
- Sender must match exactly, except the explicitly typed special-response
  prefix with the same last three sender bytes.
- A special response never confirms state.
- A query is never decoded as state.

### 7.5 FrameRecovery

Files:

- <code>core/frame_recovery.h</code>
- <code>core/frame_recovery.cpp</code>

Class:

- <code>FrameRecovery</code>

Responsibility:

- Apply the Rust-proven bounded response-recovery policy inside an already
  classified local response epoch.

Owns:

- No mutable state.

May depend on:

- <code>SenderId</code>.
- <code>FanState</code>.
- Protocol constants from <code>FrameCodec</code>.

Public API:

    class FrameRecovery final {
     public:
      static Result<RecoveredResponse, RecoveryError> recover_response(
          ByteView input,
          SenderId provisioned_sender);
    };

    enum class RecoveryQuality : std::uint8_t { Exact, Recovered };
    enum class ResponseKind : std::uint8_t { Normal, Special };

Rules:

- Fewer than six bytes is rejected.
- For longer callbacks, only the first six bytes are interpreted.
- An overlength callback is marked recovered.
- Bytes 1 through 3 must match exactly.
- Tail bytes must match.
- The first byte may be corrected only when its Hamming distance from
  <code>0xCB</code> is at most one and strictly less than its distance from
  <code>0xCE</code>.
- Special responses remain special.
- Query tails and invalid states are rejected.
- Recovery is unavailable outside a local response-classification epoch.

### 7.6 ConsensusTracker

Files:

- <code>core/consensus_tracker.h</code>
- <code>core/consensus_tracker.cpp</code>

Class:

- <code>ConsensusTracker</code>

Responsibility:

- Turn temporally independent response candidates into consensus.

Owns:

- At most one current canonical candidate group.
- Latest raw state.
- Latest non-unknown capability.
- Independent-candidate count.
- Whether any candidate is exact.
- Last candidate timestamp.

May depend on:

- <code>FanState</code>.
- <code>FrameRecovery</code>.

Public API:

    class ConsensusTracker final {
     public:
      ConsensusTracker();
      std::optional<Consensus> observe(
          const RecoveredResponse& candidate,
          MonotonicMs now_ms);
      void reset();
      ConsensusSnapshot snapshot() const;
    };

Rules:

- Candidates group by canonical lower-six-bit state.
- A different canonical state resets the group.
- Separation under 60 ms updates raw metadata but does not increase count.
- Separation at exactly 60 ms is independent.
- One exact candidate plus one independent matching candidate is sufficient.
- Three recovered-only independent matching candidates are required.
- A special response never participates and never resets a normal group.
- Backward timestamps do not rebase the group.

### 7.7 ResponseWindow

Files:

- <code>core/response_window.h</code>
- <code>core/response_window.cpp</code>

Class:

- <code>ResponseWindow</code>

Responsibility:

- Represent one immutable timing epoch with explicit origin and boundaries.

Owns:

- A typed epoch origin.
- Anchor timestamp.
- Inclusive acceptance start and end.
- Inclusive classification-tail end.
- Transaction and attempt identity when applicable.

May depend on:

- <code>core_types</code>.

Public API:

    using ResponseEpoch = variant<
        PostCommandEpoch,
        BootQueryEpoch,
        ManualQueryEpoch,
        FallbackQueryEpoch,
        RecoveryQueryEpoch,
        TailEpoch>;

    class ResponseWindow final {
     public:
      static ResponseWindow post_command(
          TransactionId transaction,
          AttemptNumber attempt,
          MonotonicMs burst_completed_ms);
      static ResponseWindow query(
          QueryPurpose purpose,
          TxToken token,
          MonotonicMs burst_started_ms);
      WindowPosition position_at(MonotonicMs now_ms) const;
      bool accepts_at(MonotonicMs now_ms) const;
      bool classifies_at(MonotonicMs now_ms) const;
      ResponseEpoch epoch() const;
    };

    enum class WindowPosition : std::uint8_t {
      BeforeAcceptance,
      Accepting,
      ClassificationTail,
      Expired
    };

Rules:

- Callers never pass a boolean such as <code>is_confirmation</code>.
- Post-command and query factories construct different variant alternatives.
- Bounds are immutable after construction.
- A fallback query creates a fresh tracker and a fresh epoch.

### 7.8 ResponseClassifier

Files:

- <code>core/response_classifier.h</code>
- <code>core/response_classifier.cpp</code>

Class:

- <code>ResponseClassifier</code>

Responsibility:

- Classify one received callback from protocol facts and current timing
  context.

Owns:

- No mutable coordinator or authority state.

May depend on:

- <code>FrameCodec</code>.
- <code>FrameRecovery</code>.
- <code>ResponseWindow</code>.

Public API:

    class ResponseClassifier final {
     public:
      ClassifiedFrame classify(
          ByteView input,
          SenderId sender,
          const ReceiveContext& context,
          MonotonicMs now_ms) const;
    };

    using ReceiveContext =
        variant<NoLocalEpoch, ActiveResponseWindow, ClassificationTail>;

    using ClassifiedFrame = variant<
        ExactOemQuery,
        LocalResponseCandidate,
        LocalTailRepeat,
        LocalTailContradiction,
        ExternalPriorityState,
        IgnoredPostCommandPreAcceptanceState,
        SpecialDiagnostic,
        InvalidOrIrrelevant>;

Rules:

- An exact matching <code>66 66</code> is always
  <code>ExactOemQuery</code>.
- It is checked before response recovery.
- The half-duplex bridge cannot hear its own query, so this classification is
  external physical priority.
- Recovery output can become <code>LocalResponseCandidate</code> only while
  the active window accepts.
- A valid frame in a classification-only tail is not an acceptance candidate.
- <code>ExternalPriorityState</code> is representable only in
  <code>NoLocalEpoch</code> or before a direct query's minimum response age.
- An exact state before a direct query's minimum response age has external
  priority because the local query cannot yet own it.
- An exact state before a post-command epoch's minimum response age is
  <code>IgnoredPostCommandPreAcceptanceState</code>: it is logged, never
  accepted, and never granted external priority.
- A post-command epoch uses its own measured bounds and is never described as
  a query epoch.
- Classification never promotes authority, ends a transaction, spends a
  budget, or sends RF.

### 7.9 CommandTransaction

Files:

- <code>core/command_transaction.h</code>
- <code>core/command_transaction.cpp</code>

Class:

- <code>CommandTransaction</code>

Responsibility:

- Own one logical request, its fixed command-attempt budget, prior-authority
  snapshot, current OFF variant, and terminal outcome.

Owns:

- Transaction ID.
- Semantic requested state.
- Current outbound command state.
- Fixed attempt limit.
- Number of physically started command bursts.
- Immutable prior authoritative observation, if one existed.
- Optional terminal outcome.

May depend on:

- <code>FanState</code>.
- Passive authority snapshot types.

Public API:

    class CommandTransaction final {
     public:
      static CommandTransaction begin(
          TransactionId id,
          FanState requested,
          std::optional<ConfirmedState> prior_authority);
      JoinDecision compare_request(FanState requested) const;
      Result<AttemptNumber, BudgetError> note_command_burst_started();
      bool may_emit_another_command() const;
      RefireCount remaining_refires() const;
      void reaim_off_to(Speed reported_speed);
      void finish(TransactionOutcome outcome);
      TransactionSnapshot snapshot() const;
    };

Rules:

- ON requests have four total command attempts: initial plus three re-fires.
- OFF requests have six total command attempts: initial plus five re-fires.
- The limit is fixed at transaction creation.
- A semantic duplicate joins and never renews budget.
- Only a physically started command burst spends one attempt.
- A query never spends command budget.
- A coordinator stall never spends command budget.
- A refused or revoked unstarted TX lease never spends command budget.
- OFF re-aiming may change only the wire variant, never the semantic request.
- No method can assign a remaining-refire count directly.
- Terminal cancellation is typed by cause.

### 7.10 AuthorityStore

Files:

- <code>core/authority_store.h</code>
- <code>core/authority_store.cpp</code>

Class:

- <code>AuthorityStore</code>

Responsibility:

- Be the only writer of state authority, timer authority, remembered speed,
  and their legal transitions.

Owns:

- One <code>StateAuthority</code> variant.
- One <code>TimerAuthority</code> variant.
- Optional remembered speed.
- Last diagnostic observation.

May depend on:

- <code>FanState</code>.
- Passive evidence and authority types.

Public API:

    class AuthorityStore final {
     public:
      AuthoritySnapshot snapshot(MonotonicMs now_ms) const;
      PriorAuthoritySnapshot capture_prior(MonotonicMs now_ms) const;
      void begin_local_command(
          TransactionId transaction,
          MonotonicMs now_ms);
      void begin_manual_revalidation(MonotonicMs now_ms);
      void promote(
          const AcceptedObservation& accepted,
          MonotonicMs now_ms);
      void invalidate(AuthorityLossReason reason, MonotonicMs now_ms);
      TimerExpiryDecision timer_estimate_expired(MonotonicMs now_ms);
      void restore_hint(
          const RestorableState& restored,
          MonotonicMs now_ms);
    };

Rules:

- No other class assigns authority.
- A refused Refresh does not call <code>begin_manual_revalidation</code>.
- An estimated timer deadline invalidates state authority; it never promotes
  OFF.
- A diagnostic external command does not become confirmed fan state.
- A known programmed timer may coexist with unknown remaining time.

The expiry decision is typed:

    enum class TimerExpiryStatus : std::uint8_t {
      NotDue,
      Due
    };

    struct TimerExpiryDecision final {
      TimerExpiryStatus status;
      std::optional<PersistenceRequest> persistence;
    };

Callers branch on <code>status</code>; an absent persistence request never has
to double as evidence that expiry was not due.

### 7.11 ObservationPolicy

Files:

- <code>core/observation_policy.h</code>
- <code>core/observation_policy.cpp</code>

Class:

- <code>ObservationPolicy</code>

Responsibility:

- Decide what a completed consensus is allowed to do.

Owns:

- No mutable state.

May depend on:

- <code>FanState</code>.
- Read-only <code>CommandTransaction</code> snapshots.
- Read-only authority snapshots.
- Typed response origins.

Public API:

    using OnRequestDecision = variant<
        ConfirmTransactionAndPromote,
        RetryWithoutPromotion,
        YieldToPossibleOem,
        ExhaustAndPromoteObservedState,
        ExhaustWithoutPromotion>;

    using OffRequestDecision = variant<
        ConfirmTransactionAndPromote,
        RetryWithoutPromotion,
        RetryAndPromoteObservedState,
        ExhaustAndPromoteObservedState,
        ExhaustWithoutPromotion>;

    using QueryDecision = variant<
        PromoteWithoutTransaction,
        DiagnosticOnly>;

    class ObservationPolicy final {
     public:
      OnRequestDecision decide_on(
          const ConsensusObservation& observation,
          const OnRequestContext& context) const;
      OffRequestDecision decide_off(
          const ConsensusObservation& observation,
          const OffRequestContext& context) const;
      QueryDecision decide_query(
          const ConsensusObservation& observation,
          const QueryAcceptanceContext& context) const;
    };

Fallback-query consensus still calls <code>decide_on</code> or
<code>decide_off</code> because it owns a transaction; only boot, manual, and
OEM/timer recovery use <code>decide_query</code>.

Rules:

- It receives a consensus, never a raw frame.
- It does not mutate the transaction or authority.
- It must name the response origin.
- Transaction contexts carry the exact value of
  <code>observation.state.has_outbound_command_marker()</code>; policy never
  reconstructs command-shapedness from raw masks.
- It must treat a prior-authority match separately from a possible OEM
  override.
- <code>OffRequestDecision</code> has no yield alternative, so yielding an OFF
  request is unrepresentable rather than merely guarded.
- It must never promote OFF while an energizing ON retry remains possible.

### 7.12 RecoveryScheduler

Files:

- <code>core/recovery_scheduler.h</code>
- <code>core/recovery_scheduler.cpp</code>

Class:

- <code>RecoveryScheduler</code>

Responsibility:

- Schedule bounded observation-only recovery after external OEM evidence or
  local estimated-timer expiry.

Owns:

- One typed <code>RecoveryCause</code>.
- Cause anchor timestamp.
- Explicit recovery phase.
- Cause-specific automatic-query budget.
- Expiry timestamp.

May depend on:

- <code>core_types</code>.

Public API:

    enum class RecoveryCause : std::uint8_t {
      OemActivity,
      EstimatedTimerExpiry
    };

    enum class RecoveryPhase : std::uint8_t {
      Inactive,
      QuietWait,
      InitialQueryPending,
      InitialResponse,
      RetryWait,
      RetryQueryPending,
      RetryResponse,
      Complete,
      Expired
    };

    class RecoveryScheduler final {
     public:
      void arm_from_oem_activity(MonotonicMs now_ms);
      void arm_from_timer_expiry(MonotonicMs now_ms);
      void cancel();
      RecoveryDue poll(MonotonicMs now_ms) const;
      void note_query_started(MonotonicMs now_ms);
      void note_consensus();
      void note_empty_window(MonotonicMs epoch_anchor_ms);
      RecoverySnapshot snapshot() const;
    };

Rules:

- One initial automatic query is allowed per OEM activity cycle.
- One logical retry is allowed if the initial query misses.
- Estimated-timer expiry allows exactly one jittered recovery query and no
  logical retry.
- A timer-expiry allowance expires if it cannot start within its named
  maximum age; it cannot become surprise RF later.
- Exact OEM evidence replaces a pending timer-expiry cause rather than adding
  budgets; OEM physical priority then governs.
- New OEM evidence restarts quiet time but does not create an unbounded chain.
- Recovery expires after 30 seconds from the latest OEM evidence.
- Authority restoration or learning cancels recovery.
- User command preempts recovery.

### 7.13 LearnMachine

Files:

- <code>core/learn_machine.h</code>
- <code>core/learn_machine.cpp</code>

Class:

- <code>LearnMachine</code>

Responsibility:

- Validate two independent OEM command presses before accepting a sender ID.

Owns:

- Optional candidate sender.
- First-candidate timestamp.
- Learn-mode deadline and mode.

May depend on:

- <code>SenderId</code>.
- <code>FanState</code>.
- Strict frame-shape rules.

Public API:

    class LearnMachine final {
     public:
      void start(LearnMode mode, MonotonicMs now_ms);
      LearnEvent observe(ByteView input, MonotonicMs now_ms);
      LearnEvent poll(MonotonicMs now_ms);
      void cancel();
      LearnSnapshot snapshot() const;
    };

Rules:

- Only exact six-byte, <code>0xCB</code>-prefixed, duplicated, valid outbound
  state commands participate.
- Queries, reports, special responses, malformed frames, and recovered frames
  never participate.
- A second matching sender at age 600 ms is still the same burst and does not
  confirm.
- A second matching sender at age 601 through 59,999 ms confirms.
- At age 60,000 ms or later, the candidate restarts.
- The two commands may encode different states.
- A different sender restarts the candidate.
- Learn events expose sender identity, never fan-state authority.

### 7.14 TransitionTable

Files:

- <code>core/transition_table.h</code>
- <code>core/transition_table.cpp</code>

This is a passive data module, not a behavior-bearing class and not a second
coordinator.
It defines compact immutable descriptors:

    struct TransitionRule {
      CoordinatorState state;
      EventKind event;
      GuardId guard;
      ActionId action;
      NextStateId next;
      RulePriority priority;
    };

It may depend only on passive IDs and enums from <code>core_types</code>.
It cannot include <code>confirmation_core.h</code>, access a context object,
invoke an action, or hold a function/member pointer.
Shared <code>constexpr</code> templates expand:

- the boot, manual, fallback, and recovery query-family lifecycle rows; and
- the ordered post-command/fallback transaction-consensus rows.

The implementation emits stable rule IDs for diagnostics and tests.
Compile-time checks reject duplicate rule IDs and nonmonotonic priority within
one state/event group.
Host tests prove first-match coverage and the intentional specialization order.

### 7.15 ConfirmationCore

Files:

- <code>core/confirmation_core.h</code>
- <code>core/confirmation_core.cpp</code>

Class:

- <code>ConfirmationCore</code>

Responsibility:

- Own the single coordinator state and apply the first matching descriptor
  from the transition table in this document.

Owns:

- One <code>CoordinatorState</code>.
- Optional current transaction.
- Optional typed response window.
- One consensus tracker for the active acceptance epoch.
- Authority store.
- Recovery scheduler.
- Learn machine.
- Optional typed deferred command.
- Monotonic transaction and TX-token allocators.
- Optional bounded radio-recovery context.

May depend on:

- Every core collaborator above.

Must not depend on:

- <code>Clock</code>.
- <code>Radio</code>.
- <code>EventSink</code>.
- ESPHome or platform headers.

Public API:

    class ConfirmationCore final {
     public:
      explicit ConfirmationCore(const CoreConfig& config);

      CoreEffects restore(
          const RestorableState& restored,
          MonotonicMs now_ms);
      CoreEffects on_radio_ready(MonotonicMs now_ms);
      CoreEffects request_state(FanState requested, MonotonicMs now_ms);
      CoreEffects request_manual_refresh(MonotonicMs now_ms);
      CoreEffects request_learn(LearnMode mode, MonotonicMs now_ms);
      CoreEffects request_forget(MonotonicMs now_ms);
      CoreEffects on_frame(ByteView input, MonotonicMs now_ms);
      CoreEffects on_tx_started(TxToken token, MonotonicMs now_ms);
      CoreEffects on_tx_complete(TxToken token, MonotonicMs now_ms);
      CoreEffects on_tx_rejected(TxToken token, MonotonicMs now_ms);
      CoreEffects on_radio_recovered(MonotonicMs now_ms);
      CoreEffects poll(MonotonicMs now_ms);

      CoreSnapshot snapshot(MonotonicMs now_ms) const;
    };

Rules:

- Each entry point processes exactly one event.
- Each entry point performs at most one state transition before returning,
  except that a global OEM-priority transition may atomically cancel owned
  subobjects.
- <code>poll</code> may issue at most one RF effect.
- The reducer selects exactly one first matching row or an explicit inert
  default; an active acceptance tracker can never fall through the
  <code>ConsensusReached</code> default.
- <code>poll</code> first derives the semantic deadline event for the current
  state, then applies that event through the same reducer.
- The reducer contains no protocol byte parsing.
- The reducer contains no consensus counting.
- The reducer contains no NVS or entity publication.

### 7.16 Clock

Files:

- <code>ports/clock.h</code>
- <code>ports/clock.cpp</code>

Class:

- <code>Clock</code>

Responsibility:

- Supply a monotonic 64-bit millisecond timestamp to the adapter.

Owns:

- Interface only.

Public API:

    class Clock {
     public:
      virtual ~Clock();
      virtual MonotonicMs now_ms() const = 0;
    };

Implementations:

- <code>EspMonotonicClock</code> extends platform rollover.
- <code>FakeClock</code> is test-only and supports explicit advance and
  backward-input tests.

Core headers do not include this interface because core entry points receive
timestamps directly.

### 7.17 Radio

Files:

- <code>ports/radio.h</code>
- <code>ports/radio.cpp</code>

Class:

- <code>Radio</code>

Responsibility:

- Send exactly one six-byte packet through a concrete ESPHome radio component.

Owns:

- Interface only.

Public API:

    class Radio {
     public:
      virtual ~Radio();
      virtual RadioSendResult send_packet(const FrameBytes& payload) = 0;
    };

Rules:

- No retry.
- No three-frame repetition.
- No 45 ms spacing.
- No confirmation logic.
- No sender learning.
- No authority mutation.
- No logging-framework calls in the interface.

### 7.18 EventSink

Files:

- <code>ports/event_sink.h</code>
- <code>ports/event_sink.cpp</code>

Class:

- <code>EventSink</code>

Responsibility:

- Receive typed core events without coupling the core to a logging framework.

Owns:

- Interface only.

Public API:

    class EventSink {
     public:
      virtual ~EventSink();
      virtual void on_core_event(const CoreEvent& event) = 0;
    };

The ESPHome sink maps typed severity and fields to ESP logs and entities.
The host sink records events for assertions.
The core never formats human-facing log strings.

### 7.19 BurstTransmitter

Files:

- <code>radio/burst_transmitter.h</code>
- <code>radio/burst_transmitter.cpp</code>

Class:

- <code>BurstTransmitter</code>

Responsibility:

- Execute one logical three-frame burst non-blockingly through <code>Radio</code>.

Owns:

- One optional TX request and token.
- Frame index 0 through 2.
- Next-frame deadline.
- Explicit transmitter phase.

May depend on:

- <code>Clock</code>.
- <code>Radio</code>.
- Passive TX types from <code>core_types</code>.

Public API:

    enum class BurstPhase : std::uint8_t {
      Idle,
      Accepted,
      SendingFrame1,
      Gap1,
      SendingFrame2,
      Gap2,
      SendingFrame3,
      Complete,
      Fault
    };

    class BurstTransmitter final {
     public:
      BurstTransmitter(Clock& clock, Radio& radio);
      TxAcceptResult accept(const TxRequest& request);
      std::optional<BurstEvent> poll();
      bool revoke_if_unstarted(TxToken token);
      BurstSnapshot snapshot() const;
    };

Rules:

- It never blocks or sleeps.
- It sends exactly three identical application packets.
- It owns the inter-frame spacing used by both radio families.
- It emits exact started, complete, rejected, and fault events.
- It accepts at most one request.
- It never automatically retries a failed packet or burst.

### 7.20 SX127xRadioAdapter

Files:

- <code>radio/sx127x_radio_adapter.h</code>
- <code>radio/sx127x_radio_adapter.cpp</code>

Class:

- <code>Sx127xRadioAdapter</code>

Responsibility:

- Translate <code>Radio::send_packet</code> to the ESPHome SX127x call.

Owns:

- A non-owning reference to the configured SX127x component.

May depend on:

- <code>Radio</code>.
- ESPHome SX127x headers.

Public API:

    class Sx127xRadioAdapter final : public Radio {
     public:
      explicit Sx127xRadioAdapter(Sx127xComponent& radio);
      RadioSendResult send_packet(const FrameBytes& payload) override;
    };

No other behavior is permitted.

### 7.21 SX126xRadioAdapter

Files:

- <code>radio/sx126x_radio_adapter.h</code>
- <code>radio/sx126x_radio_adapter.cpp</code>

Class:

- <code>Sx126xRadioAdapter</code>

Responsibility:

- Translate <code>Radio::send_packet</code> to the ESPHome SX126x call.

Owns:

- A non-owning reference to the configured SX126x component.

May depend on:

- <code>Radio</code>.
- ESPHome SX126x headers.

Public API:

    class Sx126xRadioAdapter final : public Radio {
     public:
      explicit Sx126xRadioAdapter(Sx126xComponent& radio);
      RadioSendResult send_packet(const FrameBytes& payload) override;
    };

Its source must differ from the SX127x adapter only in concrete type names and
the concrete <code>send_packet</code> invocation.

### 7.22 QuietCoolComponent

Files:

- <code>esphome/quietcool_component.h</code>
- <code>esphome/quietcool_component.cpp</code>

Class:

- <code>QuietCoolComponent</code>

Responsibility:

- Wire ESPHome lifecycle, radio RX, the clock, the burst transmitter, core
  effects, persistence, and entity publication.

Owns:

- <code>ConfirmationCore</code>.
- <code>BurstTransmitter</code>.
- References to clock, radio, event sink, and entity bridges.

May depend on:

- Core public API.
- Ports.
- Burst transmitter.
- ESPHome component and preferences APIs.

Public responsibilities:

- Call <code>core.poll(clock.now_ms())</code> from <code>loop()</code>.
- Forward received packet bytes with one timestamp.
- Forward exact burst lifecycle events.
- Execute at most the effects returned by the core.
- Convert typed snapshots to ESPHome entity updates.
- Load and store <code>RestorableState</code>.

Public API:

    class QuietCoolComponent final : public Component {
     public:
      QuietCoolComponent(
          Clock& clock,
          Radio& radio,
          EventSink& events,
          const CoreConfig& config);
      void setup() override;
      void loop() override;
      void dump_config() override;
      void on_radio_packet(ByteView packet);
      StateRequestOutcome request_state(FanState requested);
      ManualRefreshOutcome request_manual_refresh();
      LearnRequestOutcome request_learn(LearnMode mode);
      ForgetOutcome request_forget();
      CoreSnapshot snapshot() const;
    };

Forbidden responsibilities:

- Inspect protocol command bytes.
- Decide whether a frame is authoritative.
- Change retry counts.
- Decide whether Refresh is safe.
- Compute response-window age.
- Guess a timer deadline.
- Contain a copy of a state-machine transition.

### 7.23 QuietCoolFan

Files:

- <code>esphome/quietcool_fan.h</code>
- <code>esphome/quietcool_fan.cpp</code>

Class:

- <code>QuietCoolFan</code>

Responsibility:

- Translate ESPHome FanCall objects to typed core state requests and publish
  only authority snapshots received from the component.

Owns:

- A non-owning reference to <code>QuietCoolComponent</code>.

May depend on:

- <code>QuietCoolComponent</code>.
- ESPHome fan headers.

Public API:

    class QuietCoolFan final : public Component, public fan::Fan {
     public:
      explicit QuietCoolFan(QuietCoolComponent& controller);
      void setup() override;
      void dump_config() override;
      fan::FanTraits get_traits() override;
      void publish_authority(const AuthoritySnapshot& authority);

     protected:
      void control(const fan::FanCall& call) override;
    };

Rules:

- <code>control()</code> never publishes requested state optimistically.
- Publication never re-enters <code>control()</code>.
- Supported speeds are derived from authoritative capability when known and a
  safe configured default otherwise.
- The class contains no RF logic.

## 8. Allocation, error, and language policy

The core targets C++17 because that is compatible with the intended ESPHome
toolchain and gives explicit optional and variant types.
Core policy:

- No dynamic allocation after construction.
- No exceptions crossing the public API.
- No RTTI requirement.
- Fixed-capacity effect batches.
- Fixed-size frames.
- Explicit result enums instead of logging and continuing.
- No raw owning pointers.
- No callbacks that can mutate the core during a transition.
- No mutable global state.
- No static local transaction state.
- No preprocessor switches that change state semantics between SX127x and
  SX126x.

An implementation may use standard-library value types where the embedded
toolchain supports them.
If a standard type allocates, it is not permitted in the runtime core path.

## 9. Core data model

### 9.1 Transaction identity

Each accepted state request receives a monotonic <code>TransactionId</code>.
The ID is never reused during one boot.
ID exhaustion fails closed:

- no new transaction is created;
- no RF is emitted;
- and a typed diagnostic event is published.

A semantic duplicate request joins the current transaction.
A different request supersedes it.
Joining does not reset:

- attempt count;
- retry deadline;
- response epoch;
- fallback-query allowance;
- or radio-recovery budget.

### 9.2 TX identity

Each logical burst effect receives a monotonic <code>TxToken</code>.
The token identifies:

- one exact payload;
- one reason;
- one transaction and attempt when applicable;
- and one expected lifecycle.

Completion, rejection, and start callbacks with any other token are stale.
Stale callbacks:

- do not transition state;
- do not spend budget;
- do not open a response epoch;
- do not publish authority;
- and produce a diagnostic event.

### 9.3 TX reasons

The complete <code>TxReason</code> enum is:

| Value | Energizing | Owns transaction | Response origin |
|---|---:|---:|---|
| <code>BootQuery</code> | no | no | boot query |
| <code>ManualQuery</code> | no | no | manual query |
| <code>RecoveryQueryInitial</code> | no | no | OEM recovery |
| <code>RecoveryQueryRetry</code> | no | no | OEM recovery |
| <code>TimerExpiryRecoveryQuery</code> | no | no | estimated-timer recovery |
| <code>TransactionCommand</code> | possibly | yes | post-command epoch |
| <code>TransactionFallbackQuery</code> | no | yes | fallback query |

There is no generic <code>Query</code> reason.
Code that permits a TX must name one of these values.

### 9.4 Transaction outcomes

The complete <code>TransactionOutcome</code> enum is:

| Value | Meaning |
|---|---|
| <code>Confirmed</code> | accepted consensus semantically matched the request |
| <code>Exhausted</code> | all fixed command attempts completed without a match |
| <code>Superseded</code> | a different user request replaced the transaction |
| <code>CancelledByExactOemQuery</code> | a heard exact OEM query asserted physical priority |
| <code>CancelledByExternalState</code> | an <code>ExternalPriorityState</code> from NoLocalEpoch or a direct-query pre-window asserted physical priority; post-command early frames cannot produce this outcome |
| <code>YieldedToPossibleOemCommand</code> | ambiguous running mismatch caused an ON request to yield |
| <code>CancelledForLearning</code> | an explicit learn operation cancelled local work |
| <code>RadioUnavailable</code> | bounded radio recovery failed without inventing more RF attempts |

An outcome is terminal.
No terminal outcome can be changed later.

### 9.5 Prior authority is a snapshot, not a live flag

At transaction creation, <code>CommandTransaction</code> captures:

    optional<PriorAuthoritySnapshot>

The snapshot contains:

- canonical fan state;
- evidence source;
- observation timestamp;
- validity at capture;
- and state-authority revision.

The snapshot is immutable.
It is used only to distinguish:

- a running mismatch equal to the state the transaction left, which is a
  stale-echo candidate;
- from a different command-shaped running mismatch, which may be an OEM
  override.

The live <code>AuthorityStore</code> is invalidated when the local command
starts.
Invalidating live authority does not destroy the transaction's immutable prior
snapshot.
A manual Refresh cannot clear that snapshot because Refresh is refused while a
transaction is active.
This directly removes the round-4 ordering defect in which the Refresh handler
poisoned the stale-echo discriminator before its TX guard ran.

## 10. Authority model as types

### 10.1 Separation of concerns

Fan-state authority answers:

> What physical speed and running/off state may the bridge publish as current?

Timer authority answers:

> What does the bridge know about programmed duration and remaining time?

The answers have independent types and independent legal transitions.
No operation may infer timer authority from state authority.
No operation may withhold fan-state authority merely because timer remaining
time is unknown.

### 10.2 Evidence source

The complete <code>EvidenceSource</code> enum is:

| Value | Correlation anchor | Can promote state | Can anchor local timer |
|---|---|---:|---:|
| <code>BootQueryConsensus</code> | boot query TX start | yes | no |
| <code>ManualQueryConsensus</code> | manual query TX start | yes | no |
| <code>RecoveryQueryConsensus</code> | recovery query TX start | yes | no |
| <code>TimerExpiryRecoveryConsensus</code> | timer-expiry recovery query TX start | yes | no |
| <code>PostCommandConsensus</code> | command burst completion | policy-dependent | yes, if matching local timer command |
| <code>FallbackQueryConsensus</code> | fallback query TX start plus owning command attempt | policy-dependent | yes, if matching local timer command |
| <code>ExternalDiagnostic</code> | none | no | no |
| <code>RestoredHint</code> | previous boot | no | no |

### 10.3 Evidence confidence

The complete <code>EvidenceConfidence</code> enum is:

| Value | Requirement |
|---|---|
| <code>ExactBackedConsensus</code> | at least two independent candidates and at least one exact |
| <code>RecoveredOnlyConsensus</code> | at least three independent recovered candidates |

Single frames are candidates, not authority.
Raw strict validity and consensus confidence are separate facts.

### 10.4 StateAuthority

The complete state authority sum type is:

    variant<
        UnknownStateAuthority,
        RevalidatingStateAuthority,
        ConfirmedStateAuthority>

<code>UnknownStateAuthority</code> contains:

- <code>AuthorityLossReason reason</code>;
- <code>MonotonicMs since_ms</code>;
- optional last diagnostic observation;
- optional restored hint.

<code>RevalidatingStateAuthority</code> contains:

- optional previous confirmed snapshot;
- manual query request timestamp;
- the exact query transaction token once leased.

It is not authoritative for the Known diagnostic.
It exists so the UI can say revalidating without destroying diagnostic
history.
<code>ConfirmedStateAuthority</code> contains:

- validated <code>FanState state</code>;
- <code>EvidenceSource source</code>;
- <code>EvidenceConfidence confidence</code>;
- <code>MonotonicMs observed_ms</code>;
- independent candidate count;
- optional transaction and attempt IDs;
- monotonically increasing authority revision.

### 10.5 AuthorityLossReason

The complete loss-reason enum is:

| Value | Trigger |
|---|---|
| <code>Boot</code> | no current-boot consensus exists |
| <code>Unprovisioned</code> | sender ID is absent |
| <code>LocalCommandPending</code> | a state-changing request was accepted |
| <code>ManualRevalidationPending</code> | an idle Refresh was accepted |
| <code>ExactOemQuery</code> | matching OEM query was heard |
| <code>ExternalStateTraffic</code> | <code>ExternalPriorityState</code> was heard outside a local epoch or in a direct-query pre-window; never a post-command early frame |
| <code>AmbiguousOemYield</code> | ON transaction yielded to a possible physical command |
| <code>ContradictoryTail</code> | a tail contradicted accepted consensus |
| <code>ConsensusTimeout</code> | a query or exhausted transaction ended without consensus |
| <code>TransactionExhausted</code> | fixed attempt budget ended without confirmation |
| <code>EstimatedTimerDeadline</code> | local timer estimate reached its deadline |
| <code>LearningStarted</code> | identity learning invalidated old authority |
| <code>SenderChanged</code> | provisioning identity changed |
| <code>RadioUnavailable</code> | bounded radio recovery failed |
| <code>RestoredUnverified</code> | persisted hint was loaded but not confirmed this boot |

### 10.6 TimerAuthority

The complete timer authority sum type is:

    variant<
        UnknownTimerAuthority,
        NoActiveTimerAuthority,
        ProgrammedDurationAuthority,
        LocallyAnchoredTimerAuthority>

<code>UnknownTimerAuthority</code> contains:

- timer loss reason;
- timestamp since unknown.

<code>NoActiveTimerAuthority</code> contains:

- whether the confirmed state is OFF or continuous;
- evidence source;
- observation timestamp.

Both OFF and continuous have no countdown.
They are distinct fan states even though neither has a timer deadline.
<code>ProgrammedDurationAuthority</code> contains:

- one of 1, 2, 4, 8, or 12 programmed hours;
- evidence source;
- observation timestamp;
- explicit <code>RemainingTimeStatus::Unknown</code>.

This variant is used for an OEM-set or otherwise unanchored active timer.
It proves:

- the fan is running;
- the speed;
- and the programmed duration nibble.

It does not claim remaining time.
<code>LocallyAnchoredTimerAuthority</code> contains:

- programmed duration;
- owning transaction and attempt;
- command-burst completion timestamp;
- conservative expiry timestamp;
- evidence source;
- explicit anchor policy.

The initial implementation uses command-burst completion as a conservative
anchor.
The measured unsolicited report appears +705 through +807 ms later, so this
understates remaining time by under approximately one second instead of
overstating it.
That conservative bias is below the UI's minute resolution.
Changing to an actuation-estimate anchor requires new measurements and a design
change.

### 10.7 State and timer authority combinations

These are the only legal combinations:

| State authority | Timer authority | Legal |
|---|---|---:|
| Unknown | Unknown | yes |
| Revalidating | Unknown | yes |
| Confirmed OFF | NoActiveTimer(OFF) | yes |
| Confirmed continuous | NoActiveTimer(continuous) | yes |
| Confirmed timed running | ProgrammedDuration | yes |
| Confirmed timed running | LocallyAnchoredTimer | yes |
| Confirmed timed running | Unknown | temporarily during atomic promotion only; not externally visible |
| Unknown | ProgrammedDuration | no |
| Unknown | LocallyAnchoredTimer | no |
| Confirmed OFF | ProgrammedDuration | no |
| Confirmed continuous | ProgrammedDuration | no |
| Confirmed timed running | NoActiveTimer | no |

<code>AuthorityStore::promote</code> constructs state and timer variants
atomically.
No snapshot may expose the temporary internal row.

### 10.8 Legal authority transitions

Only the following transitions are legal:

| From | Event | To | Notes |
|---|---|---|---|
| Unknown | accepted idle manual Refresh | Revalidating | no transition if Refresh is refused |
| Confirmed | accepted idle manual Refresh | Revalidating(previous) | previous retained diagnostically |
| any | accepted local command | Unknown(LocalCommandPending) | transaction separately captures prior |
| any | explicit learning start | Unknown(LearningStarted) | timer also unknown |
| any | sender change or forget | Unknown(SenderChanged or Unprovisioned) | no old evidence survives |
| any | exact OEM query | Unknown(ExactOemQuery) | local transaction cancelled |
| any | externally prioritized strict state | Unknown(ExternalStateTraffic) | only outside a local epoch or in a direct-query pre-window; observation diagnostic only |
| any | contradictory tail | Unknown(ContradictoryTail) | accepted prior state is withdrawn |
| Unknown or Revalidating | accepted query consensus | Confirmed | timer constructed independently |
| Unknown | accepted matching transaction consensus | Confirmed | transaction becomes Confirmed |
| Unknown | safe nonmatching consensus during OFF transaction | Confirmed observed state | OFF transaction continues |
| Unknown | safe nonmatching consensus with future ON retry | Unknown | never publish an OFF mismatch |
| Unknown | ambiguous prior-state echo | Unknown | transaction retries; prior snapshot retained |
| Unknown | possible OEM running mismatch on ON | Unknown(AmbiguousOemYield) | transaction terminal |
| Confirmed local timer | estimated deadline | Unknown(EstimatedTimerDeadline) | never Confirmed OFF; schedule one bounded jittered observation query at the next RF-safe opportunity |
| Revalidating | manual query timeout | Unknown(ConsensusTimeout) | previous is diagnostic only |
| any | terminal radio failure | Unknown(RadioUnavailable) | no guessed state |

There is no transition:

- from an external command frame directly to Confirmed;
- from timer expiry directly to Confirmed OFF;
- from a restored state directly to Confirmed;
- or from a refused Refresh to Revalidating.

### 10.9 Publication contract

The adapter publishes the fan entity only from a
<code>ConfirmedStateAuthority</code> revision it has not published before.
It publishes diagnostics on every authority transition:

- State Known;
- Timer Known;
- Confirmed OFF;
- timer remaining;
- command confirmation status;
- and evidence source.

For <code>ProgrammedDurationAuthority</code>:

- State Known is true.
- Timer Known for programmed duration is true only if that diagnostic is
  defined as programmed-duration knowledge.
- Remaining Time Known is false.
- Timer Remaining is unavailable, not zero.
- Confirmed OFF is false because every active timer duration is nonzero.

The implementation should expose separate diagnostics:

- <code>Timer Program Known</code>;
- <code>Timer Remaining Known</code>.

This avoids recreating a single overloaded <code>timer_state_known</code>
boolean at the entity boundary.

## 11. Classification and acceptance

### 11.1 The split

The receive path is a pipeline:

    bytes
      -> strict protocol decode
      -> timing-context classification
      -> bounded recovery when eligible
      -> consensus
      -> acceptance policy
      -> authority and transaction effects

Each arrow crosses a typed API.
Classification may say:

> This is a candidate in the current post-command epoch.

It may not say:

> Therefore the fan is authoritative.

Acceptance may say:

> This consensus matches the owning transaction and can confirm it.

It never sees malformed raw bytes.

### 11.2 Classified-frame semantics

| Classification | Strictness | Consensus participation | Control effect |
|---|---|---:|---|
| ExactOemQuery | exact six-byte sender match | no | global OEM-priority cancellation |
| LocalResponseCandidate | exact or bounded recovery, inside acceptance | yes | candidate only until consensus |
| LocalTailRepeat | valid, classification-only, matches accepted state | no | quarantine only |
| LocalTailContradiction | valid, classification-only, contradicts accepted state | no | invalidate authority only; preserve the current tail exit and recovery schedule |
| ExternalPriorityState | exact strict state in NoLocalEpoch or a direct-query pre-window | no | physical priority; diagnostic only |
| IgnoredPostCommandPreAcceptanceState | exact strict state at post-command age [0, 400) ms | no | log only; never cancel, accept, recover, or spend budget |
| SpecialDiagnostic | exact/recovered special response | no | diagnostic only |
| InvalidOrIrrelevant | malformed, wrong sender, unsafe recovery, or out of policy | no | none |

### 11.3 Post-command classification

During <code>PostCommandListening</code>:

- exact matching queries are OEM priority;
- state frames inside the inclusive post-command acceptance window can become
  local candidates;
- recoverable response frames are allowed only inside that window;
- strict state frames from age 0 inclusive through 400 ms exclusive are
  <code>IgnoredPostCommandPreAcceptanceState</code>;
- those early frames are logged but cannot become
  <code>ExternalStateHeard</code>, cancel the transaction, arm recovery, or
  consume any command/fallback allowance;
- frames after acceptance and through the tail are quarantined;
- and a completed consensus closes acceptance immediately.

The classifier knows this is a <code>PostCommandEpoch</code>.
It does not infer that fact from:

- a transaction-active flag;
- a query-window flag;
- or whether the last payload was <code>0x66</code>.

The 400 ms acceptance start remains provisional because the current evidence
base is seven trials.
It is retained for this design because the earliest measured report was
+705 ms from burst end, leaving a 305 ms early margin while excluding traffic
too early to be the measured free report.
It cannot be locked for release until the >7-trial, multi-temperature hardware
gate in §20.2 passes.

### 11.4 Local-query classification

During boot, manual, fallback, or recovery query listening:

- exact matching query frames are OEM priority because the bridge is
  half-duplex;
- exact state frames before the minimum local response age have external
  priority;
- exact or bounded recovered responses inside the acceptance interval are
  candidates;
- and later frames remain in classification-only quarantine through the tail.

Each query purpose has a distinct coordinator state and a distinct epoch
variant even though it reuses the same direct-query timing constants.

### 11.5 Acceptance matrix for transaction consensus

Rows are ordered; the first matching row is the decision.
“Command-shaped” is exactly
<code>consensus.state.has_outbound_command_marker()</code> from §7.3, not a
separately reimplemented mask or inference.

| Priority | Requested | Consensus | <code>has_outbound_command_marker()</code> | Equals prior authority | Attempts remain | Decision |
|---:|---|---|---:|---:|---:|---|
| 1 | any | semantically matches | either | either | either | confirm and promote |
| 2 | ON | running mismatch | true | false | either | yield to possible OEM; no promotion |
| 3 | ON | running mismatch | true | true | yes | stale echo; retain prior snapshot diagnostically; retry; no promotion |
| 4 | any | any remaining semantic mismatch | either | either | yes | never promote OFF; for OFF, re-aim to the consensus speed and promote a running state only when policy proves it nonambiguous; for ON, retain the prior snapshot diagnostically and do not promote; retry |
| 5 | any | any remaining semantic mismatch | either | either | no | finish Exhausted; promote only evidence policy proves nonambiguous after no future energizing work remains; otherwise invalidate |
| 6 | any | no consensus | n/a | n/a | yes | one fallback query if the free report missed; otherwise retry |
| 7 | any | no consensus | n/a | n/a | no | exhaust and invalidate |

Priority 4 covers both previously missing cases:

- ON request plus OFF mismatch with attempts remaining; and
- ON request plus safe noncommand mismatch with attempts remaining.

The ordered consensus rows apply identically to post-command and fallback-query
consensus and are instantiated from one shared table template.
The origin remains visible in evidence and diagnostics.
For every valid request × consensus × attempts combination, exactly one first
matching row exists; §18.9 tests that property exhaustively.

### 11.6 Free-report miss semantics

A post-command acceptance window that closes without consensus does not:

- consume another command attempt;
- clear the transaction;
- clear OFF's remaining re-fire budget;
- invoke a response watchdog;
- or immediately emit another command.

It changes to <code>PostCommandTailWait</code>.
After the old response classification tail expires, it changes to
<code>FallbackQueryPending</code>.
Exactly one fallback query is allowed for that command attempt.
The fallback starts with a new empty <code>ConsensusTracker</code>.
Partial candidates from the free-report epoch never carry into it.
If the fallback misses:

- attempts remaining leads to <code>RetryDelay</code>;
- no attempts remaining leads to <code>Exhausted</code>.

### 11.7 Tail quarantine semantics

Acceptance closes immediately on consensus.
Classification remains active through the tail.
The state becomes <code>ResponseTailQuarantine</code> or
<code>PostCommandTailWait</code>, depending on its exit route.
During a tail:

- matching repeats are swallowed as tail repeats;
- contradictory valid frames invalidate authority but do not arm OEM recovery
  or rewrite the typed tail exit;
- exact OEM queries still have priority;
- manual Refresh is refused;
- no recovery parser output is accepted into consensus;
- and no next command is transmitted.

A missed initial OEM recovery query uses the same tail quarantine.  Its typed
<code>BeginRecoveryRetryWait</code> exit preserves classification through the
tail, then enters the classification-free <code>RecoveryRetryWait</code> state.
The estimated-timer recovery query and an OEM recovery retry have no retry exit.

A new user command may be accepted as one typed latest-wins deferred command.
It transmits only after the tail expires.
The deferred slot is not a queue.
Rapid command changes replace the deferred request and produce explicit
Superseded outcomes.
<code>PostCommandTailWait + TailExpired</code> is the only transition that can
create <code>FallbackQueryPending</code>.
<code>ResponseTailQuarantine</code> has no fallback exit.

## 12. Timing constants and boundary semantics

### 12.1 General rule

All ages use 64-bit monotonic subtraction that cannot wrap.
For an epoch anchored at <code>anchor_ms</code>:

- acceptance is <code>start_ms &lt;= age &lt;= end_ms</code>;
- the window expires only when <code>age &gt; end_ms</code>;
- classification tail includes its named end;
- and tail expiry occurs only when <code>age &gt; tail_end_ms</code>.

An event timestamp older than the active epoch anchor is rejected as stale.
It never resets an anchor or extends a deadline.

### 12.2 Response and RF timing table

| Constant | Value | Boundary | Evidence and margin |
|---|---:|---|---|
| <code>kPostCommandAcceptStartMs</code> | 400 ms | inclusive | seven-trial basis; earliest measured report +705 ms from burst end; 305 ms early margin; provisional until §20.2 thermal gate |
| <code>kPostCommandAcceptEndMs</code> | 1600 ms | inclusive | latest measured report +807 ms; 793 ms late margin |
| <code>kDirectQueryAcceptStartMs</code> | 300 ms | inclusive from <code>TxBurstStarted</code> | existing live direct-query replies +417 ms earliest; 117 ms early margin; see safe-overlap rule below |
| <code>kDirectQueryAcceptEndMs</code> | 1100 ms | inclusive from the same <code>TxBurstStarted</code> anchor | existing live direct-query replies +648 ms latest; 452 ms late margin |
| <code>kResponseTailEndMs</code> | 2500 ms | inclusive classification only | late repeats observed near +1440, +1540, and +1650 ms; at least 850 ms late margin |
| <code>kMinIndependentCandidateGapMs</code> | 60 ms | exactly 60 counts | Rust consensus policy and three-frame spacing evidence |
| <code>kCommandRetryMinDelayMs</code> | 1000 ms | eligible at or after | existing spaced re-fire backstop; actual transition is usually later because response handling owns the radio |
| <code>kInterFrameGapMs</code> | 45 ms | next frame not before | current verified burst construction; shared by both adapters |

The post-command tail uses the same conservative 2500 ms classification end as
the direct-query tail until hardware evidence supports a distinct shorter
bound.
That deliberately favors stale-tail isolation over fastest possible fallback.

### 12.3 Fallback-query timing

The fallback query may be leased only after:

- post-command acceptance has expired;
- the post-command classification tail has expired;
- no exact OEM activity holdoff is active;
- the burst transmitter is idle;
- and the transaction still owns the same attempt.

Therefore its earliest normal lease is just after +2500 ms from the completed
command burst.
Its response acceptance begins at least 300 ms after its own TX start.
The three-frame query burst takes approximately 400 ms, so the nominal direct
query window opens before burst completion.
This is deliberate and safe:

- the half-duplex radio cannot deliver an RX callback while it is transmitting;
- every query-family state remains <code>*QueryTransmitting</code>, whose RF
  permission row forbids response acceptance, until matching
  <code>TxBurstComplete</code>;
- the start anchor preserves the existing +417 through +648 ms capture basis;
- and the first physically observable candidate can therefore arrive only
  after completion even though the arithmetic predicate is already open.

Re-anchoring on burst end would require new end-relative direct-query captures
and a corresponding redesign of the 300/1100 ms constants; changing only the
anchor could discard the earliest observed replies.
This is slower than issuing the fallback at +1600 ms, but it restores the
prior-tail quarantine that the round-4 reviewer found had been narrowed.
The common successful path remains approximately 1.2 seconds and emits no
query.
This safety/latency choice is listed as an explicit implementation risk and
hardware-validation item.

### 12.4 Recovery and OEM timing table

| Constant | Value | Boundary | Evidence or policy |
|---|---:|---|---|
| <code>kOemHoldoffMs</code> | 2000 ms | active for age under 2000; inactive at 2000 | existing proven physical-priority policy |
| <code>kOemRecoveryQuietMs</code> | 3000 ms | query eligible at or after | live defect-C capture showed bridge waited until OEM exchange was quiet |
| <code>kOemRecoveryMaxAgeMs</code> | 30000 ms | expires when age exceeds 30000 | bounded relevance policy |
| <code>kOemRetryMinAfterTailMs</code> | 500 ms | inclusive lower bound | Rust bounded recovery policy |
| <code>kOemRetryJitterSpanMs</code> | 501 ms | deterministic 500 through 1000 | Rust collision-avoidance policy |
| <code>kTimerExpiryRecoveryMinMs</code> | 500 ms | inclusive lower bound | avoids immediate RF at the estimated physical stop boundary |
| <code>kTimerExpiryRecoveryJitterSpanMs</code> | 501 ms | deterministic 500 through 1000 | bounded collision avoidance without a global PRNG |
| <code>kTimerExpiryRecoveryMaxAgeMs</code> | 5000 ms | allowance expires after | prevents a delayed timer-expiry observation from becoming surprise RF |

Jitter is deterministic from stable local context.
It does not use heap allocation, a global PRNG, or wall-clock time.

### 12.5 Learning timing table

| Constant | Value | Boundary | Evidence or policy |
|---|---:|---|---|
| <code>kLearnConfirmMinAgeMs</code> | 600 ms | confirmation requires age greater than 600 | Rust learning policy; same burst repeats stay below |
| <code>kLearnConfirmMaxAgeMs</code> | 60000 ms | confirmation requires age less than 60000 | Rust learning policy |
| <code>kManualLearnWindowMs</code> | 120000 ms | expires at or after | current user-facing learn window |
| <code>kAutoLearnCeilingMs</code> | 900000 ms | no automatic re-arm at or after | current 15-minute unattended-learning ceiling |

### 12.6 Watchdog timing

Watchdogs are engineering liveness bounds, not response windows.
They never classify RF and never decide authority.
Proposed initial values:

| Constant | Value | Applies to | Effect |
|---|---:|---|---|
| <code>kTxLeaseStartWatchdogMs</code> | 500 ms | lease issued but transmitter not started | revoke unstarted lease; budget unchanged |
| <code>kTxBurstWatchdogMs</code> | 1500 ms | transmitter started but no completion | enter bounded radio recovery; a started command attempt remains spent |
| <code>kRadioRecoveryAttempts</code> | 2 | adapter reset acknowledgements | terminal RadioUnavailable after exhaustion; no extra RF |

These values need bench confirmation against both adapters before release.
They are intentionally outside <code>ResponseWindow</code>.
No response-listening state has a watchdog deadline.
Its only liveness transition is its named <code>WindowExpired</code> event.

### 12.7 Deadline event priority

When <code>poll(now_ms)</code> observes that more than one deadline has passed,
it derives exactly one event using this order:

1. <code>WindowExpired</code> for an accepting response state.
2. <code>TailExpired</code> for a classification-tail state.
3. <code>OemHoldoffExpired</code>.
4. <code>RecoveryDue</code>.
5. <code>RetryDue</code>.
6. <code>LearnWindowExpired</code>.
7. <code>TimerEstimateExpired</code>.
8. <code>TxLeaseWatchdogFired</code>.
9. <code>TxBurstWatchdogFired</code>.

Most items are mutually exclusive because the coordinator has one state.
The ordering is still contractual.
In particular, a delayed coordinator tick in
<code>PostCommandListening</code> always produces
<code>WindowExpired</code>.
It cannot produce a generic watchdog termination.
That is the structural fix for the round-4 reviewer's coordinator-stall
finding.

## 13. Coordinator state machine

### 13.1 State count

The coordinator has exactly 31 top-level states.
The implementation enum must contain these values and no generic aliases:

| # | Enum value | Meaning |
|---:|---|---|
| 1 | <code>Unprovisioned</code> | no valid sender ID; operational TX forbidden |
| 2 | <code>Idle</code> | provisioned; no local RF lifecycle active |
| 3 | <code>CommandPending</code> | a transaction is eligible when timing and radio permit |
| 4 | <code>CommandLeaseIssued</code> | exact command burst leased but not physically started |
| 5 | <code>CommandTransmitting</code> | command burst physically started |
| 6 | <code>PostCommandListening</code> | passive free-report acceptance is open or pending by age |
| 7 | <code>PostCommandTailWait</code> | free report missed; acceptance closed; tail must expire before fallback |
| 8 | <code>FallbackQueryPending</code> | exactly one transaction fallback query is eligible |
| 9 | <code>FallbackQueryLeaseIssued</code> | exact fallback query leased but not started |
| 10 | <code>FallbackQueryTransmitting</code> | fallback query burst physically started |
| 11 | <code>FallbackResponseListening</code> | fallback query response acceptance lifecycle |
| 12 | <code>RetryDelay</code> | transaction owns remaining command budget but cannot re-fire yet |
| 13 | <code>BootQueryPending</code> | one nonenergizing boot query is eligible |
| 14 | <code>BootQueryLeaseIssued</code> | boot query leased but not started |
| 15 | <code>BootQueryTransmitting</code> | boot query physically started |
| 16 | <code>BootResponseListening</code> | boot query response lifecycle |
| 17 | <code>ManualQueryPending</code> | an accepted idle Refresh is eligible |
| 18 | <code>ManualQueryLeaseIssued</code> | manual query leased but not started |
| 19 | <code>ManualQueryTransmitting</code> | manual query physically started |
| 20 | <code>ManualResponseListening</code> | manual query response lifecycle |
| 21 | <code>OemHoldoff</code> | external physical control forbids local TX |
| 22 | <code>RecoveryQuietWait</code> | authority recovery awaits three seconds of OEM quiet |
| 23 | <code>RecoveryQueryPending</code> | bounded automatic recovery query eligible |
| 24 | <code>RecoveryQueryLeaseIssued</code> | recovery query leased but not started |
| 25 | <code>RecoveryQueryTransmitting</code> | recovery query physically started |
| 26 | <code>RecoveryResponseListening</code> | recovery query response lifecycle |
| 27 | <code>RecoveryRetryWait</code> | one logical recovery retry waits through tail and jitter |
| 28 | <code>ResponseTailQuarantine</code> | acceptance closed; classification-only tail active |
| 29 | <code>LearningAwaitingFirst</code> | exclusive learn window has no candidate |
| 30 | <code>LearningAwaitingSecond</code> | exclusive learn window has one typed sender candidate |
| 31 | <code>RadioRecovery</code> | bounded adapter recovery after TX lifecycle failure |

There is no:

- <code>QueryWindow</code>;
- <code>Listening</code>;
- <code>Busy</code>;
- <code>Active</code>;
- or <code>Waiting</code>

state whose meaning must be recovered from booleans.

### 13.2 Typed state context

The enum is paired with one variant whose alternative must match the enum.
Examples:

    variant<
        UnprovisionedContext,
        IdleContext,
        CommandPendingContext,
        CommandLeaseContext,
        CommandTxContext,
        PostCommandContext,
        PostCommandTailContext,
        FallbackQueryContext,
        QueryLeaseContext,
        QueryTxContext,
        QueryResponseContext,
        RetryDelayContext,
        OemHoldoffContext,
        RecoveryContext,
        TailQuarantineContext,
        LearningContext,
        RadioRecoveryContext>

Construction and transition helpers assert the enum/variant pairing in host
tests.
Production snapshots expose the typed variant, not internal pointers.
Important contexts:

- <code>CommandTxContext</code> carries transaction ID, attempt, token, lease
  time, and physical start time.
- <code>PostCommandContext</code> carries the exact burst-completion anchor,
  fresh consensus tracker, and prior transaction snapshot.
- <code>FallbackQueryContext</code> carries the owning transaction and attempt;
  it cannot exist without them.
- <code>TailQuarantineContext</code> carries one <code>TailExit</code> variant.
- recovery-family contexts carry <code>RecoveryCause</code>, so OEM recovery
  and one-shot estimated-timer recovery share states without sharing budgets.
- <code>LearningContext</code> carries learn mode and hard deadline.
- <code>RadioRecoveryContext</code> carries one bounded recovery target.

The complete <code>TailExit</code> variant is:

    variant<
        ReturnIdle,
        BeginRetryForTransaction,
        BeginDeferredCommand,
        BeginRecoveryQuietWait,
        BeginRecoveryRetryWait>

The complete radio recovery target variant is:

    variant<
        ReissueUnstartedCommandLease,
        QuarantineStartedCommandThenRetry,
        ReissueUnstartedQueryLease,
        FinishStartedQueryAsMiss,
        ReturnIdleUnknown>

These variants replace callback-specific cleanup flags.

### 13.3 Event set

External events:

| Event | Origin |
|---|---|
| <code>ProvisioningRestored</code> | adapter restore |
| <code>RadioReady</code> | adapter setup |
| <code>CommandRequested</code> | fan/timer entity |
| <code>ManualRefreshRequested</code> | Refresh entity |
| <code>LearnRequested</code> | Learn entity or physical long press |
| <code>ForgetRequested</code> | Forget entity |
| <code>FrameReceived</code> | radio RX callback |
| <code>TxBurstStarted</code> | burst transmitter |
| <code>TxBurstComplete</code> | burst transmitter |
| <code>TxBurstRejected</code> | burst transmitter |
| <code>RadioRecovered</code> | adapter reset completion |

Derived internal events:

| Event | Derivation |
|---|---|
| <code>ExactOemQueryHeard</code> | frame classifier |
| <code>ExternalStateHeard</code> | classifier, only from <code>ExternalPriorityState</code>; never from a post-command pre-acceptance frame |
| <code>ResponseCandidate</code> | frame classifier |
| <code>ConsensusReached</code> | consensus tracker |
| <code>WindowExpired</code> | current response window and poll time |
| <code>TailExpired</code> | current tail and poll time |
| <code>RetryDue</code> | transaction deadline |
| <code>OemHoldoffExpired</code> | holdoff deadline |
| <code>RecoveryDue</code> | recovery scheduler |
| <code>LearnCandidateStarted</code> | learn machine |
| <code>Learned</code> | learn machine |
| <code>LearnWindowExpired</code> | learn machine |
| <code>TimerEstimateExpired</code> | authority store |
| <code>TxLeaseWatchdogFired</code> | lease age |
| <code>TxBurstWatchdogFired</code> | started burst age |

### 13.4 High-level diagram

    Unprovisioned
        | LearnRequested / learned sender
        v
    LearningAwaitingFirst <--> LearningAwaitingSecond
        | Learned
        v
      Idle
       | \
       |  \ RadioReady
       |   v
       |  BootQueryPending -> BootQueryLeaseIssued
       |                         -> BootQueryTransmitting
       |                         -> BootResponseListening
       |                         -> ResponseTailQuarantine -> Idle
       |
       +-- ManualRefreshRequested
       |      -> ManualQueryPending -> ManualQueryLeaseIssued
       |      -> ManualQueryTransmitting -> ManualResponseListening
       |      -> ResponseTailQuarantine -> Idle
       |
       +-- CommandRequested
              -> CommandPending -> CommandLeaseIssued
              -> CommandTransmitting -> PostCommandListening
                   | matching consensus
                   |    -> ResponseTailQuarantine -> Idle
                   |
                   | no consensus
                   v
              PostCommandTailWait -> FallbackQueryPending
              -> FallbackQueryLeaseIssued -> FallbackQueryTransmitting
              -> FallbackResponseListening
                   | match -> ResponseTailQuarantine -> Idle
                   | miss/mismatch with budget
                   v
              ResponseTailQuarantine -> RetryDelay -> CommandPending

    ExactOemQueryHeard or external-priority state
    (NoLocalEpoch/direct-query pre-window only):
        any operational state -> OemHoldoff -> RecoveryQuietWait
        -> RecoveryQueryPending -> RecoveryQueryLeaseIssued
        -> RecoveryQueryTransmitting -> RecoveryResponseListening
        -> ResponseTailQuarantine -> Idle
                         \ miss
                          -> RecoveryRetryWait -> one retry only

    Lease/TX lifecycle fault:
        any LeaseIssued or Transmitting state -> RadioRecovery
        -> typed safe resume target

### 13.5 Global transition precedence

The implementation representation is an ordered static transition table.
<code>transition_table.cpp</code> expands shared row templates, and
<code>ConfirmationCore::reduce</code> remains the sole transition owner:

1. normalize the typed event;
2. scan the current state/event group in ascending <code>RulePriority</code>;
3. ask the core-owned guard dispatcher about each passive <code>GuardId</code>;
4. apply exactly the first matching <code>ActionId</code> in the core; and
5. validate and install the descriptor's typed next-state context.

Query-family templates remove duplicated lifecycle rows without merging the
named coordinator states.
The shared transaction-consensus template gives §13.9 and §13.10 identical
first-match structure.
The passive table cannot mutate state, so this representation satisfies both
the single-transition-owner rule and the §5.1 line limits.

The following are checked before state-local frame handling:

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| any provisioned operational state | ExactOemQueryHeard | exact six bytes, exact sender, duplicated <code>66</code> | revoke local lease, terminate transaction as CancelledByExactOemQuery, clear consensus, invalidate both authorities, arm recovery, stamp holdoff | OemHoldoff |
| OemHoldoff | ExactOemQueryHeard | exact sender | restart holdoff and recovery quiet anchors | OemHoldoff |
| RecoveryQuietWait | ExactOemQueryHeard | exact sender | restart holdoff and recovery quiet anchors | OemHoldoff |
| any recovery query state | ExactOemQueryHeard | exact sender | revoke local recovery work, restart physical-priority cycle | OemHoldoff |
| ResponseTailQuarantine | ExactOemQueryHeard | exact sender | discard tail exit and deferred local RF, invalidate authority, restart recovery | OemHoldoff |
| LearningAwaitingFirst | ExactOemQueryHeard | learn mode active | feed as ignored learning traffic; no local work exists | LearningAwaitingFirst |
| LearningAwaitingSecond | ExactOemQueryHeard | learn mode active | leave candidate unchanged | LearningAwaitingSecond |
| Unprovisioned | FrameReceived | no provisioned sender | pass only to LearnMachine if learning is active; never operationally classify | Unprovisioned or learning state |

An exact OEM query is never swallowed by:

- an acceptance window;
- a classification tail;
- a TX token;
- or consensus.

### 13.6 Provisioning and boot transitions

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| Unprovisioned | ProvisioningRestored | valid sender | install sender, restore nonauthoritative hint, mark authority RestoredUnverified | Idle |
| Unprovisioned | ProvisioningRestored | invalid or absent | emit unprovisioned diagnostic | Unprovisioned |
| Idle | RadioReady | first ready notification this boot | create one BootQuery request | BootQueryPending |
| Unprovisioned | RadioReady | any | emit Unprovisioned; no TX | Unprovisioned |
| any | RadioReady | already handled | diagnostic only | same state |
| BootQueryPending | poll | radio idle and no holdoff | issue typed boot-query lease | BootQueryLeaseIssued |
| BootQueryLeaseIssued | TxBurstStarted | matching token | record direct-query physical-start anchor; acceptance remains closed | BootQueryTransmitting |
| BootQueryLeaseIssued | TxBurstRejected | matching token | no retry; authority remains unknown | Idle |
| BootQueryLeaseIssued | TxLeaseWatchdogFired | matching live lease | revoke lease; enter bounded reset without RF budget | RadioRecovery |
| BootQueryTransmitting | TxBurstComplete | matching token | begin direct-query response lifecycle | BootResponseListening |
| BootQueryTransmitting | TxBurstWatchdogFired | matching started token | response timing untrusted; request radio reset | RadioRecovery |
| BootResponseListening | ResponseCandidate | inside inclusive direct-query window | update fresh tracker | BootResponseListening |
| BootResponseListening | ConsensusReached | accepted query consensus | atomically promote state/timer authority; close acceptance with ReturnIdle exit | ResponseTailQuarantine |
| BootResponseListening | WindowExpired | no consensus | authority Unknown(ConsensusTimeout); close acceptance | ResponseTailQuarantine |
| BootResponseListening | ExternalStateHeard | exact pre-window external priority | invalidate; arm OEM recovery | OemHoldoff |

The boot query is one-shot.
No failure path schedules another boot query.

### 13.7 Idle and manual Refresh transitions

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| Idle | ManualRefreshRequested | sender provisioned, radio idle, no hidden recovery work | AuthorityStore begins revalidation; create manual query | ManualQueryPending |
| Unprovisioned | ManualRefreshRequested | no sender | refuse Unprovisioned; no authority mutation; no deferred TX | Unprovisioned |
| any non-Idle operational state | ManualRefreshRequested | any | refuse Busy or Holdoff by named state; no authority mutation; no deferred TX | same state |
| LearningAwaitingFirst | ManualRefreshRequested | learning active | refuse Learning | LearningAwaitingFirst |
| LearningAwaitingSecond | ManualRefreshRequested | learning active | refuse Learning | LearningAwaitingSecond |
| ManualQueryPending | poll | radio idle | issue typed manual-query lease | ManualQueryLeaseIssued |
| ManualQueryLeaseIssued | TxBurstStarted | matching token | bind Revalidating authority to token; record physical-start anchor; acceptance remains closed | ManualQueryTransmitting |
| ManualQueryLeaseIssued | TxBurstRejected | matching token | authority becomes Unknown(ConsensusTimeout); no auto retry | Idle |
| ManualQueryLeaseIssued | TxLeaseWatchdogFired | live unstarted token | revoke; bounded radio recovery target ReissueUnstartedQueryLease | RadioRecovery |
| ManualQueryTransmitting | TxBurstComplete | matching token | begin direct response lifecycle | ManualResponseListening |
| ManualQueryTransmitting | TxBurstWatchdogFired | matching token | timing trust lost; bounded radio recovery target FinishStartedQueryAsMiss | RadioRecovery |
| ManualResponseListening | ResponseCandidate | inside inclusive direct-query window | update fresh tracker | ManualResponseListening |
| ManualResponseListening | ConsensusReached | accepted | atomically replace prior authority; clear OEM recovery | ResponseTailQuarantine |
| ManualResponseListening | WindowExpired | no consensus | authority Unknown(ConsensusTimeout); no retry | ResponseTailQuarantine |
| ManualResponseListening | ExternalStateHeard | exact pre-window state | cancel local query, invalidate, arm recovery | OemHoldoff |

Manual Refresh is one-shot.
Refusal never queues a query to run later.

### 13.8 New command transitions

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| Idle | CommandRequested | sender valid and state encodable | capture prior authority, allocate transaction, invalidate live authority | CommandPending |
| Unprovisioned | CommandRequested | no sender | refuse Unprovisioned; no RF | Unprovisioned |
| CommandPending | CommandRequested | semantic duplicate | join existing transaction; do not renew budget | CommandPending |
| CommandPending | CommandRequested | different semantic state | finish old as Superseded; create replacement with inherited defensible prior snapshot | CommandPending |
| CommandLeaseIssued | CommandRequested | semantic duplicate | join; retain token and budget | CommandLeaseIssued |
| CommandLeaseIssued | CommandRequested | different and lease unstarted | revoke old lease; supersede; create replacement | CommandPending |
| CommandTransmitting | CommandRequested | semantic duplicate | join current transaction | CommandTransmitting |
| CommandTransmitting | CommandRequested | different; RF may have started | finish old as Superseded; store replacement as typed deferred command; request radio quiescence | RadioRecovery |
| PostCommandListening | CommandRequested | semantic duplicate | join current transaction | PostCommandListening |
| PostCommandListening | CommandRequested | different | supersede; poison acceptance; defer replacement until current tail end | ResponseTailQuarantine |
| PostCommandTailWait | CommandRequested | different | supersede old; replace fallback exit with deferred command after tail | ResponseTailQuarantine |
| FallbackQueryPending | CommandRequested | different | supersede old; cancel unleased fallback; defer until old tail safe point if still active | ResponseTailQuarantine or CommandPending |
| FallbackQueryLeaseIssued | CommandRequested | different and unstarted | revoke fallback; supersede; defer until classification-safe boundary | ResponseTailQuarantine |
| FallbackQueryTransmitting | CommandRequested | different | supersede; defer replacement until query tail | ResponseTailQuarantine |
| FallbackResponseListening | CommandRequested | different | supersede; close acceptance; defer until query tail | ResponseTailQuarantine |
| RetryDelay | CommandRequested | semantic duplicate | join without deadline change | RetryDelay |
| RetryDelay | CommandRequested | different | supersede and replace transaction; preserve safe earliest-TX boundary | RetryDelay or CommandPending |
| BootQueryPending | CommandRequested | query not leased | cancel boot query; begin transaction | CommandPending |
| ManualQueryPending | CommandRequested | query not leased | cancel revalidation; begin transaction with prior diagnostic snapshot | CommandPending |
| RecoveryQuietWait | CommandRequested | sender valid | cancel recovery cycle; begin transaction | CommandPending |
| RecoveryQueryPending | CommandRequested | query not leased | cancel recovery; begin transaction | CommandPending |
| OemHoldoff | CommandRequested | sender valid | store latest deferred command; do not shorten holdoff | OemHoldoff |
| ResponseTailQuarantine | CommandRequested | no active transaction | store or replace latest deferred command; no TX before tail | ResponseTailQuarantine |
| LearningAwaitingFirst | CommandRequested | an existing sender is provisioned | cancel learning; begin transaction | CommandPending |
| LearningAwaitingSecond | CommandRequested | an existing sender is provisioned | cancel learning candidate; begin transaction | CommandPending |
| either learning state | CommandRequested | no provisioned sender | refuse Unprovisioned | same learning state |

Where a table row permits two next states, the guard is an explicit
<code>WindowPosition</code> variant:

- expired means <code>CommandPending</code>;
- any other position means <code>ResponseTailQuarantine</code>.

It is not a boolean.
Every accepted <code>CommandRequested</code> action also cancels a pending
timer-expiry recovery allowance; the command transaction's bounded observation
path takes its place.

### 13.9 Command TX and passive report transitions

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| CommandPending | poll | radio idle, holdoff absent, retry minimum met, budget available | issue command lease with next attempt but do not spend yet | CommandLeaseIssued |
| CommandLeaseIssued | TxBurstStarted | matching token | spend exactly one command attempt; record physical start | CommandTransmitting |
| CommandLeaseIssued | TxBurstRejected | matching token | revoke unspent attempt intent; schedule bounded adapter recovery or retry lease | RadioRecovery |
| CommandLeaseIssued | TxLeaseWatchdogFired | matching unstarted token | revoke; command budget unchanged | RadioRecovery |
| CommandTransmitting | TxBurstComplete | matching token and active transaction/attempt | anchor post-command epoch at completion; reset tracker | PostCommandListening |
| CommandTransmitting | TxBurstWatchdogFired | matching token | keep started attempt spent; timing untrusted; reset radio then quarantine before retry | RadioRecovery |
| PostCommandListening | FrameReceived(IgnoredPostCommandPreAcceptanceState) | same epoch and age at least 0 but under 400 ms | publish rate-limited diagnostic only; do not cancel, accept, arm recovery, or spend any allowance | PostCommandListening |
| PostCommandListening | ResponseCandidate | age inside 400 through 1600 inclusive and epoch identity matches | update post-command tracker | PostCommandListening |
| PostCommandListening | ResponseCandidate | candidate does not create consensus | publish diagnostic candidate only | PostCommandListening |
| PostCommandListening | ConsensusReached | semantic match | finish Confirmed; atomically promote authority; set tail exit ReturnIdle | ResponseTailQuarantine |
| PostCommandListening | ConsensusReached | ON running mismatch; <code>has_outbound_command_marker()</code> true; not prior match | finish YieldedToPossibleOemCommand; invalidate; arm recovery after tail | ResponseTailQuarantine |
| PostCommandListening | ConsensusReached | ON running mismatch; <code>has_outbound_command_marker()</code> true; equals prior; attempts remain | retain prior snapshot diagnostically; do not promote; set tail exit BeginRetry | ResponseTailQuarantine |
| PostCommandListening | ConsensusReached | any remaining mismatch; attempts remain | never promote OFF; for OFF re-aim to consensus speed and promote running only if nonambiguous; for ON retain prior diagnostically without promotion; set BeginRetry | ResponseTailQuarantine |
| PostCommandListening | ConsensusReached | any remaining mismatch; no attempts remain | finish Exhausted; promote only if policy proves evidence nonambiguous; otherwise invalidate; set ReturnIdle | ResponseTailQuarantine |
| PostCommandListening | WindowExpired | no consensus, regardless of lateness of poll | preserve transaction and all unspent refires; enter the sole fallback-tail route | PostCommandTailWait |
| PostCommandListening | FrameReceived(LocalTailRepeat) | no consensus, age over 1600 through 2500 before a poll derived WindowExpired | close acceptance with the same miss semantics, preserve transaction/budget, swallow repeat | PostCommandTailWait |
| PostCommandListening | FrameReceived(LocalTailContradiction) | no consensus, age over 1600 through 2500 before a poll derived WindowExpired | close acceptance with the same miss semantics; invalidate authority only; preserve transaction/budget | PostCommandTailWait |

The four <code>ConsensusReached</code> outcomes after semantic match are ordered
specialization, specialization, generic attempts-remain, generic exhausted.
They are instantiated from the same row template and in the same order as
§13.10.
The <code>WindowExpired</code> row is selected before any watchdog rule.

### 13.10 Post-command miss and fallback transitions

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| PostCommandTailWait | FrameReceived(LocalTailRepeat) | same epoch through +2500 inclusive | swallow; no consensus | PostCommandTailWait |
| PostCommandTailWait | FrameReceived(LocalTailContradiction) | valid contradiction | invalidate authority only; keep transaction and fallback route unchanged | PostCommandTailWait |
| PostCommandTailWait | TailExpired | same transaction and attempt still active | create exactly one fallback query | FallbackQueryPending |
| FallbackQueryPending | poll | radio idle, no holdoff, fallback allowance unspent | mark allowance spent; issue fallback lease | FallbackQueryLeaseIssued |
| FallbackQueryLeaseIssued | TxBurstStarted | matching token | record fresh direct-query physical-start anchor; reset tracker; acceptance remains closed | FallbackQueryTransmitting |
| FallbackQueryLeaseIssued | TxBurstRejected | matching unstarted token | no command budget spent; bounded radio recovery may reissue same one query | RadioRecovery |
| FallbackQueryLeaseIssued | TxLeaseWatchdogFired | live unstarted token | revoke and recover; fallback allowance remains tied to same query identity | RadioRecovery |
| FallbackQueryTransmitting | TxBurstComplete | matching token | open direct-query response lifecycle | FallbackResponseListening |
| FallbackQueryTransmitting | TxBurstWatchdogFired | started token | treat response timing as untrusted miss after radio recovery; never create a second fallback query | RadioRecovery |
| FallbackResponseListening | ResponseCandidate | age 300 through 1100 inclusive | update fresh fallback tracker | FallbackResponseListening |
| FallbackResponseListening | ConsensusReached | semantic match | finish Confirmed; atomically promote authority; set tail exit ReturnIdle | ResponseTailQuarantine |
| FallbackResponseListening | ConsensusReached | ON running mismatch; <code>has_outbound_command_marker()</code> true; not prior match | finish YieldedToPossibleOemCommand; invalidate; arm recovery after tail | ResponseTailQuarantine |
| FallbackResponseListening | ConsensusReached | ON running mismatch; <code>has_outbound_command_marker()</code> true; equals prior; attempts remain | retain prior snapshot diagnostically; do not promote; set tail exit BeginRetry | ResponseTailQuarantine |
| FallbackResponseListening | ConsensusReached | any remaining mismatch; attempts remain | never promote OFF; for OFF re-aim to consensus speed and promote running only if nonambiguous; for ON retain prior diagnostically without promotion; set BeginRetry | ResponseTailQuarantine |
| FallbackResponseListening | ConsensusReached | any remaining mismatch; no attempts remain | finish Exhausted; promote only if policy proves evidence nonambiguous; otherwise invalidate; set ReturnIdle | ResponseTailQuarantine |
| FallbackResponseListening | WindowExpired | attempts remain | preserve remaining budget; set BeginRetry exit | ResponseTailQuarantine |
| FallbackResponseListening | WindowExpired | no attempts remain | finish Exhausted; invalidate | ResponseTailQuarantine |
| FallbackResponseListening | ExternalStateHeard | exact pre-window state | cancel local transaction for physical priority | OemHoldoff |

The five ordered <code>ConsensusReached</code> rows, including semantic match,
are structurally identical to §13.9 and come from the same table template.
Only the listening-state ID and response-origin evidence differ.

### 13.11 Tail and retry transitions

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| ResponseTailQuarantine | FrameReceived(LocalTailRepeat) | matches accepted or tracked tail state | swallow | ResponseTailQuarantine |
| ResponseTailQuarantine | FrameReceived(LocalTailContradiction) | valid contradiction | invalidate authority only; preserve <code>TailExit</code> exactly, including BeginRetry | ResponseTailQuarantine |
| ResponseTailQuarantine | TailExpired | exit ReturnIdle, live timer-expiry recovery is pending at age at most 5000, and no deferred command | clear epoch/tracker; retain one-shot recovery cause | RecoveryQuietWait |
| ResponseTailQuarantine | TailExpired | exit ReturnIdle, timer-expiry recovery age is over 5000, and no deferred command | clear epoch/tracker; discard expired allowance | Idle |
| ResponseTailQuarantine | TailExpired | exit ReturnIdle, no timer-expiry recovery pending, and no deferred command | clear epoch/tracker | Idle |
| ResponseTailQuarantine | TailExpired | exit BeginRetry and retry minimum already met | clear epoch | CommandPending |
| ResponseTailQuarantine | TailExpired | exit BeginRetry and retry minimum not met | retain transaction and exact deadline | RetryDelay |
| ResponseTailQuarantine | TailExpired | exit BeginDeferredCommand | install newest deferred transaction | CommandPending |
| ResponseTailQuarantine | TailExpired | exit BeginRecoveryQuietWait | preserve latest OEM activity anchor | RecoveryQuietWait |
| ResponseTailQuarantine | TailExpired | exit BeginRecoveryRetryWait | clear epoch/tracker; preserve scheduler retry deadline | RecoveryRetryWait |
| RetryDelay | RetryDue | transaction active and budget remains | no budget spent yet | CommandPending |
| RetryDelay | RetryDue | transaction missing or exhausted | defensive invariant event; no TX | Idle |

The transition out of a tail uses <code>TailExit</code>.
No code asks several flags what should happen next.
<code>BeginFallbackForTransaction</code> is not a <code>TailExit</code>
alternative; the only fallback transition is
<code>PostCommandTailWait + TailExpired</code> in §13.10.
A local tail contradiction follows the Rust reference: it withdraws authority
but cannot divert a budgeted retry into OEM recovery.

### 13.12 External state and OEM recovery transitions

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| Idle | ExternalStateHeard | exact strict state outside local epoch | retain diagnostic only, invalidate authority, arm holdoff and recovery | OemHoldoff |
| any operational nonlearning state except PostCommandListening | ExternalStateHeard | classifier produced <code>ExternalPriorityState</code> from NoLocalEpoch or a direct-query pre-window | cancel local work, retain diagnostic, invalidate, arm recovery | OemHoldoff |
| OemHoldoff | OemHoldoffExpired | deferred command exists | cancel recovery cycle; install deferred command | CommandPending |
| OemHoldoff | OemHoldoffExpired | no deferred command | retain recovery schedule | RecoveryQuietWait |
| RecoveryQuietWait | RecoveryDue | cause OemActivity, age at least 3000 and at most 30000, radio idle | mark initial query pending | RecoveryQueryPending |
| RecoveryQuietWait | RecoveryDue | cause OemActivity and age over 30000 | expire recovery | Idle |
| RecoveryQuietWait | RecoveryDue | cause EstimatedTimerExpiry, jitter deadline reached, age at most 5000, radio idle, and no holdoff | mark the one timer-expiry query pending | RecoveryQueryPending |
| RecoveryQuietWait | RecoveryDue | cause EstimatedTimerExpiry and age over 5000 | discard unused allowance; remain Unknown | Idle |
| RecoveryQueryPending | poll | radio idle and no holdoff | issue cause-specific initial, retry, or timer-expiry reason from scheduler | RecoveryQueryLeaseIssued |
| RecoveryQueryLeaseIssued | TxBurstStarted | matching token | record direct-query physical-start anchor; acceptance remains closed | RecoveryQueryTransmitting |
| RecoveryQueryLeaseIssued | TxBurstRejected | matching token | bounded radio recovery; do not silently allocate new logical retry | RadioRecovery |
| RecoveryQueryLeaseIssued | TxLeaseWatchdogFired | unstarted | revoke and recover same logical query | RadioRecovery |
| RecoveryQueryTransmitting | TxBurstComplete | matching token | begin direct response lifecycle | RecoveryResponseListening |
| RecoveryQueryTransmitting | TxBurstWatchdogFired | started | recover radio and count current logical query as a miss | RadioRecovery |
| RecoveryResponseListening | ResponseCandidate | inside direct-query acceptance | update fresh tracker | RecoveryResponseListening |
| RecoveryResponseListening | ConsensusReached | accepted | promote authority; cancel recovery cycle; set ReturnIdle | ResponseTailQuarantine |
| RecoveryResponseListening | WindowExpired | initial OEM query and logical retry available | scheduler computes tail plus deterministic jitter; set exit BeginRecoveryRetryWait | ResponseTailQuarantine |
| RecoveryResponseListening | WindowExpired | OEM retry query, no retry available, or timer-expiry cause | end recovery Unknown; set ReturnIdle; timer-expiry cause never retries | ResponseTailQuarantine |
| ResponseTailQuarantine | TailExpired | exit BeginRecoveryRetryWait and recovery still live | clear classification epoch; preserve scheduler deadline | RecoveryRetryWait |
| RecoveryRetryWait | RecoveryDue | within 30-second max age | mark retry query pending | RecoveryQueryPending |
| RecoveryRetryWait | RecoveryDue | expired | cancel recovery | Idle |
| any recovery state | CommandRequested | valid sender | user command preempts automatic observation work | CommandPending or safe tail |
| any recovery state | LearnRequested | valid request | cancel recovery and local TX | LearningAwaitingFirst |

One OEM activity cycle emits at most two automatic query bursts:

- initial;
- and one retry.

One estimated-timer-expiry cycle emits at most one automatic query burst.
User command, accepted consensus, learning, or exact OEM evidence cancels or
replaces that allowance; none of those events renews it.

### 13.13 Learning transitions

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| any operational state | LearnRequested | manual or authorized auto mode | cancel/revoke local work, invalidate authority, cancel recovery, start hard deadline | LearningAwaitingFirst |
| Unprovisioned | LearnRequested | valid mode | start hard deadline | LearningAwaitingFirst |
| LearningAwaitingFirst | FrameReceived | not an exact learnable command | ignore without changing candidate | LearningAwaitingFirst |
| LearningAwaitingFirst | LearnCandidateStarted | exact learnable command | store typed sender and timestamp | LearningAwaitingSecond |
| LearningAwaitingSecond | FrameReceived | malformed/query/report/special | ignore without changing candidate | LearningAwaitingSecond |
| LearningAwaitingSecond | LearnCandidateStarted | different sender | replace candidate and anchor | LearningAwaitingSecond |
| LearningAwaitingSecond | FrameReceived | same sender age at most 600 | same burst; ignore | LearningAwaitingSecond |
| LearningAwaitingSecond | Learned | same sender age over 600 and under 60000 | replace sender, emit persistence request, clear old authority/recovery | Idle |
| LearningAwaitingSecond | LearnCandidateStarted | same sender age at least 60000 | restart anchor without learning | LearningAwaitingSecond |
| LearningAwaitingFirst | LearnWindowExpired | existing sender remains | cancel learning | Idle |
| LearningAwaitingSecond | LearnWindowExpired | existing sender remains | discard candidate | Idle |
| either learning state | LearnWindowExpired | no sender | discard candidate | Unprovisioned |
| either learning state | ForgetRequested | any | erase provisioning, persist erase, restart explicitly authorized auto learn if policy says so | LearningAwaitingFirst or Unprovisioned |

Learning states never send RF.

### 13.14 Forget transitions

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| any state | ForgetRequested | explicit user action | revoke local lease, terminate active transaction, clear epochs, clear recovery, invalidate authorities, erase sender and restore hint, request durable persistence | Unprovisioned |
| Unprovisioned | ForgetRequested | already absent | idempotent persistence erase and diagnostic | Unprovisioned |

Auto-learn after Forget is a separate explicit policy effect.
The erase transition itself cannot transmit.

### 13.15 Radio-recovery transitions

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| any LeaseIssued | TxLeaseWatchdogFired | lease never started | revoke token; preserve logical RF allowance and command budget; request reset | RadioRecovery |
| any Transmitting | TxBurstWatchdogFired | token started | preserve command attempt as spent if command; mark response timing untrusted; request reset | RadioRecovery |
| RadioRecovery | RadioRecovered | target ReissueUnstartedCommandLease and attempts remain | decrement recovery budget only; do not spend command budget | CommandPending |
| RadioRecovery | RadioRecovered | target QuarantineStartedCommandThenRetry | establish conservative tail from last safe timestamp | ResponseTailQuarantine |
| RadioRecovery | RadioRecovered | target ReissueUnstartedQueryLease, purpose TimerExpiryRecoveryQuery, and cause age over 5000 | discard expired one-shot allowance; invalidate authority | Idle |
| RadioRecovery | RadioRecovered | target ReissueUnstartedQueryLease | return same logical query purpose without creating another allowance | corresponding QueryPending |
| RadioRecovery | RadioRecovered | target FinishStartedQueryAsMiss | apply that query purpose's empty-window rule | ResponseTailQuarantine or RecoveryRetryWait |
| RadioRecovery | RadioRecovered | target ReturnIdleUnknown | invalidate authority | Idle |
| RadioRecovery | watchdog or reset rejection | recovery attempts remain | request next bounded reset; no RF | RadioRecovery |
| RadioRecovery | watchdog or reset rejection | recovery attempts exhausted | finish transaction RadioUnavailable if any; invalidate; publish fault; no further automatic RF | Idle or Unprovisioned |
| RadioRecovery | ExactOemQueryHeard | exact sender | physical priority replaces recovery target | OemHoldoff |
| RadioRecovery | LearnRequested | explicit | abandon radio recovery and local work | LearningAwaitingFirst |

The forced-watchdog hardware exercise must demonstrate the first successful
recovery row.

### 13.16 Timer-expiry transitions

Timer estimate expiry is an authority transition, not an operational RF state.

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| Idle | TimerEstimateExpired | locally anchored timer deadline reached | authority Unknown(EstimatedTimerDeadline); publish unavailable remaining; arm exactly one deterministic 500–1000 ms jittered recovery query, but emit no immediate TX | RecoveryQuietWait |
| any command/query/tail operational state | TimerEstimateExpired | deadline reached while other work active and no OEM recovery owns priority | invalidate timer and fan authority; arm one pending timer-expiry recovery allowance without changing operation or command/fallback budget | same state |
| OemHoldoff or OEM recovery state | TimerEstimateExpired | external physical-priority recovery already owns observation | invalidate timer and fan authority; coalesce into existing OEM recovery; create no additional query allowance | same state |
| learning states | TimerEstimateExpired | old identity authority already unknown | diagnostic only | same state |

There is no estimated-expiry transition to Confirmed OFF.
A pending timer-expiry allowance runs only at the next typed RF-safe boundary:
an Idle expiry enters <code>RecoveryQuietWait</code> directly, while a tail uses
the pending-recovery row in §13.11.
User command preempts it, exact OEM evidence replaces it, accepted consensus
cancels it, and age over 5000 ms discards it.
Cancellation is an atomic side effect of every accepted-consensus action, not
a second transition.
Thus expiry closes the loop once without creating a new retry family or
renewable RF source.

### 13.17 Stale and irrelevant events

For completeness, every state follows these default rows:

| State | Event | Guard | Action | Next state |
|---|---|---|---|---|
| any | TX lifecycle callback | token does not match live state context | emit StaleTxCallback diagnostic | same state |
| any | ConsensusReached | no active acceptance tracker for this state | emit InvalidInternalEvent in debug/test; no production side effect | same state |
| any | WindowExpired | state is not one of four response-listening states or PostCommandListening | ignore as stale derived event | same state |
| any | TailExpired | state has no tail context | ignore as stale derived event | same state |
| any | FrameReceived(InvalidOrIrrelevant) | any | optional rate-limited diagnostic only | same state |
| any | FrameReceived(SpecialDiagnostic) | any | diagnostic only | same state |
| any | ResponseCandidate | candidate epoch identity differs | reject stale candidate | same state |
| any | ManualRefreshRequested | state is not Idle | typed refusal; no deferred work | same state |

The <code>ConsensusReached</code> default is unreachable from
<code>PostCommandListening</code> and <code>FallbackResponseListening</code>
with a live tracker: their shared ordered template covers semantic match, both
ON specializations, every remaining mismatch with attempts, and every
remaining mismatch without attempts.
No unspecified event may emit RF.
No unspecified event may change authority.

### 13.18 State-specific RF permission matrix

This table is a compile-time review aid.

| State | May lease command | May lease query | May accept response | May classify tail |
|---|---:|---:|---:|---:|
| Unprovisioned | no | no | no | no |
| Idle | no | no | no | no |
| CommandPending | yes | no | no | no |
| CommandLeaseIssued | no | no | no | no |
| CommandTransmitting | no | no | no | no |
| PostCommandListening | no | no | post-command only | yes |
| PostCommandTailWait | no | no | no | yes |
| FallbackQueryPending | no | fallback only | no | no |
| FallbackQueryLeaseIssued | no | no | no | no |
| FallbackQueryTransmitting | no | no | no | no |
| FallbackResponseListening | no | no | direct-query only | yes |
| RetryDelay | no | no | no | no |
| BootQueryPending | no | boot only | no | no |
| BootQueryLeaseIssued | no | no | no | no |
| BootQueryTransmitting | no | no | no | no |
| BootResponseListening | no | no | direct-query only | yes |
| ManualQueryPending | no | manual only | no | no |
| ManualQueryLeaseIssued | no | no | no | no |
| ManualQueryTransmitting | no | no | no | no |
| ManualResponseListening | no | no | direct-query only | yes |
| OemHoldoff | no | no | no | no |
| RecoveryQuietWait | no | no | no | no |
| RecoveryQueryPending | no | recovery only | no | no |
| RecoveryQueryLeaseIssued | no | no | no | no |
| RecoveryQueryTransmitting | no | no | no | no |
| RecoveryResponseListening | no | no | direct-query only | yes |
| RecoveryRetryWait | no | no | no | no (retry jitter only; classification ended at the preceding tail) |
| ResponseTailQuarantine | no | no | no | yes (including a missed initial OEM recovery query before its typed retry-wait exit) |
| LearningAwaitingFirst | no | no | no | no |
| LearningAwaitingSecond | no | no | no | no |
| RadioRecovery | no | no | no | no |

Only four pending-state rows may lease RF.
Every call site must switch on the exact enum value.

## 14. Historical defect mapping

### 14.1 Defect A: speed-to-speed commands confirmed stale pre-command state

Observed failure:

- a query was transmitted before the fan actuated;
- it returned the prior running state;
- and yield logic treated that stale state as an OEM override.

Structural prevention:

- <code>CommandTransmitting</code> can transition only to
  <code>PostCommandListening</code> on burst completion.
- <code>PostCommandListening</code> has no query permission.
- The RF permission matrix forbids a query in that state.
- A missed free report transitions through <code>PostCommandTailWait</code>
  before <code>FallbackQueryPending</code>.
- The earliest fallback is after actuation and after the old tail.
- <code>CommandTransaction</code> keeps an immutable prior-authority snapshot.
- A prior-state running mismatch is a stale-echo retry decision, not an OEM
  yield.

Regression property:

> From command TX start through post-command tail expiry, no
> <code>0x66</code> TX effect is possible except the named fallback transition
> after the tail.

### 14.2 Defect B: OEM-set timer made fan-state authority unrecoverable

Observed failure:

- one boolean required both known fan state and a locally anchored timer;
- an OEM timer could never produce remaining-time authority;
- therefore speed/running state was withheld forever.

Structural prevention:

- <code>StateAuthority</code> and <code>TimerAuthority</code> are separate sum
  types.
- Query consensus over a running timer always permits confirmed speed/running
  when otherwise safe.
- An unanchored timer produces
  <code>ProgrammedDurationAuthority</code>.
- Remaining time is explicitly Unknown.
- The legal-combination table permits this state.
- No authority transition checks a boolean named locally anchored before
  promoting fan state.

Regression property:

> Manual or recovery query consensus over an OEM-set timer yields State Known
> true and Remaining Time Known false.

### 14.3 Defect C: OEM remote wedge was not a bridge defect

Field evidence:

- the bridge TX count did not move during the remote's retry storm;
- its recovery query occurred only after three seconds of quiet;
- remote power cycling cleared the problem.

Design consequence:

- exact OEM query priority cancels local work globally;
- <code>OemHoldoff</code> and <code>RecoveryQuietWait</code> are explicit;
- the core emits no RF for at least two seconds of holdoff and no automatic
  recovery query before three seconds of quiet;
- automatic recovery is bounded to two query bursts;
- host tests assert silence during repeated OEM traffic.

This remains a non-defect classification.
The design does not claim to repair a wedged OEM remote.

### 14.4 Defect D: free report misclassified as OEM traffic

Observed failure:

- the fan's unsolicited report arrived before the delayed confirmation query;
- no local query epoch was open;
- so the report cancelled the local transaction as external OEM traffic;
- confirmation took the later OEM-recovery path.

Structural prevention:

- exact command burst completion creates a
  <code>PostCommandEpoch</code>.
- the coordinator enters <code>PostCommandListening</code>.
- the +400 through +1600 ms inclusive window classifies eligible state frames
  as local candidates.
- an exact state at post-command age 0 through 399 ms is a typed ignored early
  frame, not external priority, and cannot cancel the transaction.
- consensus can directly confirm the transaction.
- no query is required in the common path.
- diagnostic source remains <code>PostCommandConsensus</code>, never OEM.

Regression property:

> A matching three-frame report at +705, +805, and +905 ms from burst end
> confirms with one command burst and zero query bursts.

Companion boundary property:

> A strict state frame at +0 or +399 ms is logged and ignored; the transaction,
> all remaining OFF refires, and its one fallback allowance remain intact.

### 14.5 Round-4 Refresh regression

Reverted failure:

- <code>cl_query_window</code> acquired a second meaning: passive free-report
  listening;
- the manual-query guard still interpreted it as own query in flight;
- Refresh at +800 ms transmitted <code>66</code> before actuation;
- and the handler cleared prior state before the guard.

Structural prevention:

- passive free-report listening is
  <code>CoordinatorState::PostCommandListening</code>.
- own manual query states are
  <code>ManualQueryPending</code>,
  <code>ManualQueryLeaseIssued</code>,
  <code>ManualQueryTransmitting</code>, and
  <code>ManualResponseListening</code>.
- only <code>Idle</code> accepts <code>ManualRefreshRequested</code>.
- every other state returns a typed refusal.
- refusal occurs before any authority method is called.
- no Refresh is deferred.
- the RF permission matrix forbids queries from every post-command state.
- prior authority lives inside the transaction snapshot and cannot be cleared
  by a refused Refresh.

Regression property:

> Press Refresh at +800 ms after a command burst; assert no TX effect, no
> authority revision, no prior-snapshot change, and state remains
> PostCommandListening.

### 14.6 Round-4 coordinator-stall finding

Reverted-risk shape:

- a delayed periodic coordinator woke after both a response boundary and a
  generic watchdog threshold;
- watchdog termination won;
- a recoverable miss became terminal and OFF budget was destroyed.

Structural prevention:

- response states have no watchdog transition.
- <code>poll</code> derives <code>WindowExpired</code> first.
- <code>PostCommandListening + WindowExpired</code> preserves the transaction
  and goes to <code>PostCommandTailWait</code>.
- attempt budget is a transaction object and is unchanged.

Regression property:

> Advance fake time from burst completion directly to +5000 ms without polling;
> one poll produces the fallback path, not a terminal result, and OFF still has
> five unspent re-fires after its initial started burst.

### 14.7 Round-4 prior-tail finding

Reviewer concern:

- removing the old tail quarantine narrowed stale-repeat separation.

Structural prevention:

- acceptance and tail classification are distinct.
- all accepted and missed epochs retain classification through +2500 ms.
- fallback query lease waits until the prior post-command tail expires.
- new command TX is deferred through an existing tail.
- each new acceptance epoch starts with an empty tracker.

Regression property:

> A late old-state frame at +2499 ms is classified as tail only and cannot
> seed the next fallback-query consensus.

## 15. Safety invariants

INV-01 through INV-23 must each have at least one direct host test.
INV-24 is a hardware/integration gate.
INV-25 has a host-testable publication-decision half and an ESPHome integration
gate.
Invariant IDs remain stable review references.

### INV-01: OFF never yields

An OFF transaction never takes
<code>YieldedToPossibleOemCommand</code> from a state-frame mismatch.
The policy API enforces this structurally:
<code>OffRequestDecision</code> omits <code>YieldToPossibleOem</code>.
It may still be cancelled by:

- a heard exact OEM <code>66 66</code> query;
- an explicit superseding user request;
- explicit learning or Forget;
- or terminal radio unavailability.

An ambiguous running mismatch re-aims OFF to the reported speed when valid and
continues within its original budget.

### INV-02: OFF re-fire budget is nonrenewable and fully spendable

An OFF transaction has:

- one initial command attempt;
- five re-fire attempts;
- six total command attempts.

After the initial command burst starts, <code>remaining_refires()</code> is
exactly five.
The following never decrement it:

- free-report window expiry;
- fallback-query TX;
- fallback-response expiry;
- coordinator stalls;
- stale callbacks;
- refused Refresh;
- an unstarted TX lease watchdog;
- and response-tail quarantine.

### INV-03: ON retry budget is fixed

An ON transaction has one initial command attempt and three re-fires.
A joined duplicate never renews it.

### INV-04: exact OEM query wins

A strictly decoded query for the provisioned sender cancels:

- the active transaction;
- any pending command or fallback;
- any manual, boot, or recovery query;
- any unstarted TX lease;
- and any deferred local TX.

It enters <code>OemHoldoff</code>.
No local RF occurs before holdoff and recovery rules permit it.

### INV-05: unprovisioned means RF silent

No state reachable from <code>Unprovisioned</code> may emit a TX effect.
Learning is receive-only.

### INV-06: one physical burst at a time

At most one live <code>TxToken</code> exists.
<code>BurstTransmitter</code> accepts at most one logical burst.
Command and query packets cannot interleave.

### INV-07: bounded command RF

Per logical transaction:

| Request | Maximum command bursts | Maximum fallback-query bursts | Maximum logical bursts | Maximum six-byte packet sends |
|---|---:|---:|---:|---:|
| ON | 4 | 4 | 8 | 24 |
| OFF | 6 | 6 | 12 | 36 |

Each logical burst is exactly three packet sends.
The fallback maximum assumes every command attempt misses its free report.
No automatic path exceeds these bounds.
External callers can create new transactions, but each receives a new fixed
budget and explicit ID.

### INV-08: bounded OEM recovery RF

One OEM activity cycle emits at most:

- one initial recovery query;
- one retry recovery query;
- six total packet sends.

New OEM activity restarts the physical-priority cycle.
It does not mutate an already spent query into a fresh unbounded budget.

### INV-09: manual Refresh is idle-only

Only <code>Idle</code> accepts Refresh.
All other 30 states refuse it.
A refusal:

- sends no RF;
- creates no pending work;
- changes no authority revision;
- changes no transaction snapshot;
- and changes no timing anchor.

### INV-10: no pre-actuation query

No query is emitted as part of a transaction before:

- its post-command acceptance window closes;
- its post-command tail expires;
- and <code>FallbackQueryPending</code> is reached.

### INV-11: a missed free report degrades exactly once

For each command attempt, closing
<code>PostCommandListening</code> without consensus creates exactly one
fallback-query allowance.
That allowance cannot recursively create another fallback.
If the fallback misses, the next automatic state-changing RF is the next
budgeted command attempt or none.

### INV-12: consensus does not leak across epochs

Every post-command, fallback, manual, boot, and recovery epoch starts with an
empty <code>ConsensusTracker</code>.
Tail frames never seed a later tracker.

### INV-13: classification is not acceptance

No <code>ClassifiedFrame</code> directly changes authority.
Only <code>ConsensusReached</code> followed by one of the typed
<code>ObservationPolicy</code> decisions can promote.

### INV-14: external commands are not confirmation

A strict state frame classified as <code>ExternalPriorityState</code> outside a
local epoch or in a direct-query pre-window is diagnostic physical priority.
It invalidates current authority but does not publish its state as confirmed.
A post-command pre-acceptance frame is instead typed ignored evidence and does
not invalidate or cancel.

### INV-15: timer authority cannot block fan authority

Accepted consensus for an active timer promotes speed and running state even
when remaining time is unknowable.

### INV-16: estimated expiry never confirms OFF

Timer deadline processing changes state authority to Unknown and never
publishes OFF.
It emits no immediate RF, but schedules at most one deterministic jittered
recovery query with no retry and a 5000 ms maximum age.

### INV-17: future energizing work cannot publish OFF

An ON transaction with another possible command attempt never publishes an OFF
mismatch as current authority.

### INV-18: coordinator stall remains recoverable

For any response-listening state, advancing time across its end and then
calling <code>poll</code> produces <code>WindowExpired</code>.
It never produces a terminal watchdog transition.

### INV-19: only started command bursts spend attempts

Lease issue, queueing, rejection before start, and lease watchdog do not spend
command budget.
A burst-start event spends one attempt because partial RF may already be
physical.

### INV-20: stale callbacks are inert

A wrong or duplicate token cannot:

- release the live token;
- open or close an epoch;
- spend a budget;
- or publish authority.

### INV-21: time cannot move backward

A timestamp older than an owned anchor is rejected.
It cannot rebase consensus, extend a window, postpone recovery expiry, or make
a timer authoritative.

### INV-22: learning is exclusive and receive-only

Learning cancels local operational work and never sends RF.
Learning evidence never becomes fan-state authority.

### INV-23: persistence cannot resurrect RF

Restore never creates:

- a transaction;
- a TX token;
- a response epoch;
- a timer deadline;
- a recovery cycle;
- or confirmed current authority.

### INV-24 integration gate: radio families are behaviorally identical

Given the same core events and timestamps, SX127x and SX126x integrations
produce the same core effects.
Only one packet-send method differs.
Host tests can prove shared core and <code>BurstTransmitter</code> behavior, but
actual FIFO completion, RX callback, and on-air parity require §20.8 hardware
validation on both families.

### INV-25 hybrid invariant/gate: publication is confirmation-driven

An ESPHome FanCall never directly changes the published fan entity.
Only a new confirmed-authority revision does.
The adapter-level double in §18 verifies the decision boundary; the real
ESPHome callback/publication path remains a Stage-5 integration gate.

## 16. Outside-world interfaces and event flow

### 16.1 Clock flow

The adapter reads <code>Clock::now_ms()</code> once per external callback.
That same value is passed through the full core entry point.
It is not read again mid-transition.
This makes one transition temporally atomic.
The ESP clock adapter must:

- extend 32-bit wrap to 64 bits;
- survive repeated equal timestamps;
- and never use wall-clock time.

### 16.2 RX flow

For either radio family:

1. ESPHome invokes its radio packet callback.
2. The thin YAML/action or component callback forwards bytes and metadata.
3. <code>QuietCoolComponent</code> takes one clock sample.
4. It calls <code>ConfirmationCore::on_frame</code>.
5. The core classifies, may update consensus, and returns effects.
6. The component applies events and entity snapshots.

RSSI and SNR remain diagnostics.
They do not change protocol validity or authority in the initial design.

### 16.3 TX flow

1. <code>ConfirmationCore::poll</code> returns at most one
   <code>RequestTxBurst</code> effect.
2. The component offers it to <code>BurstTransmitter</code>.
3. Acceptance does not by itself spend command budget.
4. The transmitter emits <code>TxBurstStarted</code> immediately before the
   first physical packet send.
5. The core validates the token and spends a command attempt if applicable.
6. The shared transmitter sends exactly three packets with shared spacing.
7. It emits <code>TxBurstComplete</code> after frame three completes.
8. For a command, the core anchors the post-command epoch at completion.
9. For a query, the core opens response acceptance at completion while
   retaining the physical-start timing anchor used by the measured direct-query
   constants; QueryTransmitting itself cannot accept RX.

If the radio API cannot expose physical completion, the adapter must define
its <code>send_packet</code> result so the shared BurstTransmitter can derive
the earliest time the hardware guarantees the third packet has left the FIFO.
That hardware-specific result requires bench validation.

### 16.4 EventSink flow

Core events are typed records such as:

- transaction accepted;
- transaction confirmed;
- request refused with reason and state;
- OEM priority asserted;
- candidate observed;
- consensus reached;
- authority changed;
- timer remaining unknown;
- recovery scheduled;
- watchdog fired;
- and invariant violation.

The event sink chooses:

- log severity;
- human-readable text;
- ESPHome text-sensor mapping;
- and rate limiting.

No logger call appears in core headers or sources.

### 16.5 ESPHome adapter thinness test

A code-review rule and host-side dependency check must verify that
<code>esphome/</code> files do not reference:

- acceptance-window constants;
- consensus counts;
- attempt-limit numeric literals;
- canonical state masks;
- yield policy;
- or authority transitions.

The only permitted adapter-side timing constants are component scheduling and
radio-driver timing needed by <code>BurstTransmitter</code>.

## 17. Persistence

### 17.1 Persisted data

The proposed versioned restore record contains:

| Field | Persist | Core use after restore |
|---|---:|---|
| schema version | yes | migration and validation |
| sender ID | yes | provisioning |
| compiled-seed suppression policy | yes | preserve explicit Forget |
| remembered speed | yes, only on change | construct protocol-faithful OFF when no current authority |
| last confirmed canonical state | optional | diagnostic restored hint only |
| last speed capability | optional | diagnostic hint only |
| checksum or preferences validation | adapter-owned | reject corrupted record |

The sender ID value is never written into this design or tests.

### 17.2 Volatile-only data

The following never survive reboot or OTA:

- confirmed current authority;
- timer remaining authority;
- timer anchor or deadline;
- active transaction;
- prior-authority transaction snapshot;
- command attempt count;
- fallback allowance;
- TX token;
- transmit lease;
- response epoch;
- consensus candidates;
- OEM holdoff;
- OEM recovery cycle;
- learning candidate;
- deferred command;
- and radio-recovery context.

### 17.3 Why last confirmed state is not restored as authority

The fan may change while the bridge is offline.
A persisted timestamp is not comparable to the new monotonic boot epoch.
Therefore a restored state is:

    RestoredHint

not:

    ConfirmedStateAuthority

The adapter may show the hint in a diagnostic text sensor.
It must publish State Known false until current-boot consensus.

### 17.4 RestorableState API

The core expresses storage needs without knowing NVS:

    struct RestorableState {
      RestoreSchemaVersion version;
      optional<SenderId> sender;
      SeedPolicy seed_policy;
      optional<Speed> remembered_speed;
      optional<RestoredObservationHint> observation_hint;
    };

    using PersistenceRequest = variant<
        SaveProvisioning,
        EraseProvisioning,
        SaveRememberedSpeed>;

The adapter:

- validates and loads preferences;
- calls <code>core.restore</code>;
- performs persistence effects;
- debounces writes;
- and requests a durable sync after explicit Learn or Forget.

The core:

- validates semantic compatibility;
- returns Unprovisioned for invalid sender data;
- marks restored observation unverified;
- and never calls a storage API.

### 17.5 OTA and reboot behavior

After reboot or OTA:

- sender provisioning survives;
- explicit Forget remains forgotten;
- no command re-fires;
- no timer expiry is guessed;
- no old query response can be accepted;
- State Known is false;
- and one bounded boot query is scheduled after radio readiness.

If boot query misses, the core remains unknown and RF-silent until:

- a user command;
- manual Refresh;
- or external OEM evidence followed by bounded recovery.

## 18. Host test plan

### 18.1 Framework

Use the offline hand-rolled test harness and plain Makefile as the canonical
implementation-phase host toolchain.
Rationale:

- native Apple Clang support on macOS;
- readable behavior and boundary assertions;
- generated loops for protocol and transition matrices;
- no ESPHome runtime;
- deterministic suite labels by invariant or historical defect;
- and ordinary C++17 compilation of the same core sources used on device.

Use the system C++ compiler and require no network access or downloaded test
dependency.  This toolchain is already proven by the Stage 1 checkpoint and
therefore replaces the earlier Catch2/CMake pin.
The implementation phase may add build files and test scaffolding only after
this design is approved.
No such files belong to this phase.

### 18.2 Test doubles

Host support provides:

- <code>FakeClock</code> with set, advance, wrap-extension, and deliberate
  backward-input operations;
- <code>FakeRadio</code> recording exact packet bytes and configurable
  rejection;
- <code>FakeBurstTransmitter</code> capable of start, complete, duplicate,
  stale, and watchdog timelines;
- <code>RecordingEventSink</code>;
- <code>FakePreferences</code>;
- <code>FakeFanEntity</code> and <code>FakeFanCall</code> for the narrow
  adapter contract, recording requests and publications without an ESPHome
  runtime;
- a read-only <code>TransitionTableInspector</code> exposing stable rule IDs,
  priorities, and template origin to tests without exposing coordinator state;
- and a timeline harness that records state, authority, TX effects, and event
  sequence at each timestamp.

The timeline harness calls public APIs only.
It must not inspect private fields.

### 18.3 Rust port baseline

The Rust workspace currently lists 334 named tests across all crates.
The <code>quietcool-core</code> crate itself lists 83 named tests:

- 1 internal control test;
- 8 consensus tests;
- 43 external control tests;
- 8 domain tests;
- 9 frame tests;
- 6 learn tests;
- 6 recovery tests;
- and 2 compile-fail documentation tests.

The C++ core port uses the 83 core tests as the direct semantic source.
The wider workspace's 334 tests remain a parity inventory for adapter and
integration scenarios.
The implementation should not claim 334 one-for-one C++ core tests unless it
actually ports the non-core cases too.
Parameterized protocol matrices count as concrete generated cases and must
report their input on failure.

### 18.4 Domain and protocol test list

Port these concrete groups:

1. Sender ID round-trips big-endian bytes and display order.
2. Non-<code>0xCB</code> operational sender prefixes are rejected.
3. Sender ID has no valid default or zero sentinel.
4. Speed accepts only 1, 2, and 3.
5. Duration accepts only 0, 1, 2, 4, 8, 12, and 15.
6. Every speed/duration command produces the golden canonical matrix.
7. Observed neutral OFF is valid.
8. Outbound neutral OFF without remembered speed is rejected.
9. Metadata becomes capability without changing canonical equality.
10. All OFF variants are semantically equal.
11. Every invalid duration nibble is rejected.
12. Running with speed zero is rejected.
13. Query encoding uses exact six-byte wire order.
14. Every valid state command round-trips.
15. Encoding normalizes outbound metadata only.
16. Golden observed frames preserve their raw state byte.
17. Exact query is never state.
18. Special response is distinct and never confirming.
19. Strict decode rejects every wrong length in the selected boundary set.
20. Strict decode rejects wrong byte order, wrong sender, and unequal tails.
21. Strict decode rejects invalid state and special query.
22. Metadata masking never changes sender bytes.

### 18.5 Recovery and consensus test list

1. Exact normal and special responses remain distinct.
2. Every possible first byte is exhaustively checked against recovery rules.
3. A one-bit <code>0xCB</code> error is recovered only when unambiguous.
4. Headers ambiguous with or closer to <code>0xCE</code> are rejected.
5. Short, wrong-ID, unequal-tail, invalid-state, and query inputs are rejected.
6. Overlength callbacks use only the first six bytes and are marked recovered.
7. Two independent candidates reach consensus when one is exact.
8. Three independent candidates are required when all are recovered.
9. Candidate gap 59 ms does not count.
10. Candidate gap 60 ms counts.
11. Backward time cannot rebase the gap.
12. Canonical state change resets count, exactness, and timing.
13. Latest raw byte and latest known capability are preserved.
14. Debounced repeat updates metadata but not confidence.
15. Special responses neither participate nor reset a normal group.
16. Reset creates a completely empty next epoch.

### 18.6 Learning test list

1. First valid command starts a candidate.
2. Same sender at exactly 600 ms does not confirm.
3. Same sender at 601 ms confirms.
4. Same sender at 59,999 ms confirms.
5. Same sender at 60,000 ms restarts.
6. Different valid sender restarts.
7. Second command may encode a different state.
8. Query does not participate.
9. Malformed frame does not participate.
10. Report-shaped frame does not participate.
11. Special response does not participate.
12. Learn event contains sender identity only.
13. Manual learn expires at its hard deadline.
14. Auto learn cannot re-arm beyond 15 minutes.
15. Learning sends no RF.

### 18.7 State-machine and authority test list

Port every named Rust control scenario, including:

- one provisioned boot query and repeated-ready silence;
- unprovisioned boot silence;
- safe sender replacement only when fully quiet;
- exact TX token ownership and harmless duplicate completion;
- normal and OFF nonrenewable budgets;
- semantic duplicate join;
- different-request supersession;
- old-epoch poisoning on supersession;
- elapsed-time behavior near the integer maximum;
- post-command confirmation without a query;
- inclusive post-command boundaries;
- post-command state frames at +0 and +399 ms logged and ignored without
  cancellation, recovery, or budget change;
- exactly one fallback after a miss;
- fresh fallback consensus after a partial free report;
- OEM query cancellation during post-command listening;
- fallback query re-anchoring;
- classification-only post-command tail;
- matching consensus cancellation of remaining attempts;
- exact-backed versus recovered-only thresholds;
- inclusive query-response boundaries;
- OFF re-aiming;
- active-timer mismatch state authority;
- exact OEM holdoff ending at 2000 ms;
- OEM invalidation of an already leased token;
- wrong-ID and recovered queries not taking OEM priority;
- one-shot manual query and inert refusal;
- boot/manual authority without a transaction;
- <code>ExternalPriorityState</code> diagnostic-only observation plus
  cancellation behavior outside a local epoch/direct-query pre-window;
- authority invalidation at each actual retry;
- transaction poisoning of an older manual epoch;
- no OFF publication while an ON retry remains;
- dropped ON command producing OFF consensus with attempts remaining selecting
  retry, never promotion or default handling;
- ON safe noncommand mismatch with attempts remaining selecting retry;
- classification-tail quarantine delaying a next command;
- local tail contradiction preserving BeginRetry and every other typed tail
  exit while invalidating authority only;
- ON yield versus OFF never-yield;
- prior-authority stale-echo retry;
- authority inheritance across supersession;
- bounded OEM initial query and one logical retry;
- authority or learning clearing OEM recovery;
- physical query re-anchoring recovery;
- manual Refresh consuming an eligible recovery observation slot only when
  actually accepted from Idle;
- externally prioritized state cancellation outside a post-command epoch;
- pre-window state external priority for a local query;
- local versus unanchored timer authority;
- local timer expiry scheduling one 500–1000 ms jittered recovery query,
  cancelling it on accepted consensus/user command, and discarding it after
  5000 ms;
- manual Refresh replacing authority only after consensus;
- and backward query timestamps never stranding a transaction.

Add C++-specific state coverage:

- every enum value has a valid context variant;
- invalid enum/context pair is rejected in a test-only constructor;
- every state appears in the RF permission matrix test;
- all 30 non-Idle states refuse Refresh;
- every LeaseIssued state handles lease watchdog;
- every Transmitting state handles burst watchdog;
- every response state handles a late poll as WindowExpired;
- every tail exit variant has a test;
- no ResponseTailQuarantine rule can reach FallbackQueryPending;
- PostCommandTailWait + TailExpired is the sole fallback predecessor;
- every generated transition rule has one stable ID and monotonic priority;
- the four query-family instantiations share the same lifecycle template while
  retaining distinct state IDs;
- <code>FakeFanCall</code> produces a core request but zero publications, and
  <code>FakeFanEntity</code> publishes only a new confirmed-authority revision;
- and every transaction outcome is reachable through its named event.

### 18.8 Historical regression timeline scripts

#### REG-A: speed-to-speed stale reply

Timeline:

1. At t=0, establish authoritative LOW continuous by manual-query consensus.
2. At t=10000, request HIGH continuous.
3. Start and complete command burst; call completion t=10400.
4. Assert state is PostCommandListening.
5. Inject LOW frames at t=11105 and t=11165, matching prior authority.
6. Assert no OEM yield and no query TX.
7. Complete LOW consensus if desired; assert it is stale-echo retry evidence.
8. Inject matching HIGH consensus in the next valid command attempt.
9. Assert Confirmed HIGH.

#### REG-B: OEM timer separates authorities

Timeline:

1. Start Idle with unknown authority.
2. Request manual Refresh at t=0.
3. Start query at t=10 and complete its burst.
4. Inject exact HIGH one-hour candidates at t=427 and t=487.
5. Assert StateAuthority is Confirmed HIGH running.
6. Assert TimerAuthority is ProgrammedDuration one hour.
7. Assert remaining time is Unknown.
8. Assert Confirmed OFF is false.

#### REG-D: unsolicited report is the common confirmation

Timeline:

1. Request LOW at t=0.
2. Complete command burst at t=400.
3. Inject matching report candidates at t=1105 and t=1165.
4. Assert consensus source is PostCommandConsensus.
5. Assert transaction is Confirmed near the measured path.
6. Assert exactly one command logical burst.
7. Assert zero query logical bursts.
8. Assert later matching tail frame causes no state change.

#### REG-R4-REFRESH: Refresh at +800 ms

Timeline:

1. Complete a command burst at t=400.
2. At t=1200, while PostCommandListening, request manual Refresh.
3. Assert outcome Busy with state name PostCommandListening.
4. Assert no TX effect.
5. Assert TX history contains no <code>0x66</code>.
6. Assert prior-authority snapshot is unchanged.
7. Assert live authority revision is unchanged by the refusal.
8. Continue matching free-report consensus and assert normal confirmation.

#### REG-R4-STALL: delayed coordinator

Timeline:

1. Start OFF with six-attempt budget.
2. Complete initial command burst at t=400.
3. Do not poll or inject RX through t=5400.
4. Call poll once.
5. Assert derived event is WindowExpired, followed by safe tail/fallback
   progression.
6. Assert transaction is not terminal.
7. Assert five OFF re-fires remain spendable.
8. Assert no watchdog event.

#### REG-TAIL: old repeat cannot seed fallback

Timeline:

1. Complete command burst at t=400.
2. Let free-report acceptance miss.
3. Inject one old-state frame at anchor +2499.
4. Assert tail classification only.
5. Expire tail and start fallback query.
6. Inject one matching fallback candidate.
7. Assert no consensus until a second independent candidate arrives.

#### REG-C: repeated OEM traffic keeps bridge silent

Timeline:

1. Hear exact OEM query at t=0.
2. Repeat OEM query/command evidence every 1000 ms for 11 seconds.
3. Assert no local TX throughout.
4. Stop OEM traffic.
5. Assert no recovery query before three seconds of quiet.
6. Assert one query becomes eligible at the boundary.

### 18.9 Property and model tests

Add generated tests for:

- arbitrary event sequences never exceeding one live TX token;
- arbitrary time jumps never increasing command budget;
- arbitrary repeated Refresh events never producing TX outside Idle;
- arbitrary malformed frames never changing authority;
- arbitrary exact OEM queries always reaching OemHoldoff from operational
  states;
- all OFF variants never yielding to state-frame mismatch;
- every valid requested semantic state × every valid raw consensus
  <code>FanState</code> (therefore its actual
  <code>has_outbound_command_marker()</code> value) × prior relation {absent,
  equal, unequal} × attempts value {zero, nonzero} selecting exactly one first
  matching transaction-consensus row;
- that exhaustive matrix selecting the same ordered rule template for
  PostCommandListening and FallbackResponseListening;
- every ON mismatch with attempts remaining, including OFF and safe
  noncommand mismatches, selecting retry rather than the invalid-internal-event
  default;
- every state/event group having strictly ordered priorities and either a
  matching explicit row or the documented inert default;
- and RF effect count never exceeding the transaction bound.

A small independent reference model should track:

- coordinator enum;
- attempts spent;
- TX count;
- whether sender exists;
- and whether exact OEM priority is active.

It need not duplicate protocol parsing.

### 18.10 Host commands expected in implementation

The canonical validation command is:

    make -C tests/cpp test

The suite must run on macOS without:

- ESPHome;
- PlatformIO;
- Arduino;
- ESP-IDF;
- a radio;
- network access after dependencies are provisioned;
- or sender secrets.

## 19. What host tests cannot verify

Host tests cannot verify:

- 433.920 MHz tuning;
- FSK bitrate or deviation;
- sync word and preamble behavior;
- radio FIFO completion semantics;
- the physical three-frame airtime;
- +705 through +807 ms free-report timing;
- 1154 through 1257 ms request-to-report timing;
- approximately 1.2-second actuation;
- direct-query response timing;
- RF collisions;
- receiver corruption distribution;
- SX126x board pin assumptions;
- actual SX127x/SX126x FIFO-completion and RX-callback parity;
- the real ESPHome <code>FanCall</code>-to-entity publication path beyond the
  narrow adapter double;
- or byte-identical physical ambiguity beyond the modeled rule.

Those facts require hardware.

## 20. On-hardware validation protocol

No implementation is complete until the following controlled protocol passes.

### 20.1 Capture setup

- Log monotonic event timestamps, state names, transaction/attempt IDs, TX
  tokens, typed classifications, consensus, and authority revisions.
- Count logical bursts and individual packet sends separately.
- Keep sender IDs redacted.
- Retain raw relative timing and state bytes.
- Run on the known SX127x unit first.
- Run SX126x only on a bench unit until its board is physically verified.

### 20.2 Common-path trials

The existing 400 ms acceptance-start choice is based on seven trials and is
not locked for release by that sample alone.
Before locking <code>kPostCommandAcceptStartMs</code>, collect more than seven
new burst-end-relative trials across thermal conditions: at least nine total,
with at least three each on a cold-start enclosure, at nominal stabilized
temperature, and on a warmed enclosure.
If any new earliest report materially consumes the current 305 ms early
margin, reopen the constant and its host boundaries in the design rather than
locking 400 ms by fiat.

The trial set must cover at least:

- OFF to LOW;
- LOW to HIGH;
- HIGH to LOW;
- LOW to LOW no-op;
- HIGH to OFF;
- and an explicit timer command.

For each:

- confirm one command burst;
- confirm no query burst on a received free report;
- record burst-end to first report;
- record consensus time;
- and confirm authority source PostCommandConsensus.

### 20.3 Forced free-report miss

Use one controlled method:

- temporarily shrink the post-command acceptance window in a dedicated
  validation build;
- or detune/disable RX only for that acceptance interval.

Do not change production constants silently.
Observe:

1. command burst;
2. free-report miss;
3. classification-tail wait;
4. exactly one fallback query;
5. direct-query response consensus;
6. transaction confirmation;
7. no premature command re-fire.

Then force both free report and fallback response to miss.
Observe one next budgeted command attempt and no recursive query.

### 20.4 Forced watchdog trip

Instrument the burst adapter in a validation build to suppress one completion
callback after start.
Observe:

- one watchdog event;
- one bounded radio reset;
- preserved unspent attempt budget;
- conservative tail handling for the started command;
- successful later confirmation;
- and no unbounded reset or RF loop.

Repeat with an unstarted lease and verify no command attempt is spent.

### 20.5 Physical OEM priority

During:

- PostCommandListening;
- FallbackResponseListening;
- RetryDelay;
- and RecoveryResponseListening,

press the OEM remote so its exact query is heard.
Observe:

- local cancellation;
- OemHoldoff;
- no further command re-fire;
- and recovery only after quiet.

### 20.6 Refresh timing

Press Refresh:

- immediately after a local command;
- at +800 ms from command request/burst-relative equivalent;
- during post-command tail;
- during fallback query;
- and when Idle.

Only the Idle press may transmit.

### 20.7 Timer validation

Set a timer with the OEM remote.
Refresh.
Observe:

- speed/running authority confirmed;
- programmed duration visible;
- remaining time unavailable;
- Confirmed OFF false.

Set a local timer.
Confirm that its conservative countdown is locally anchored.
At estimated expiry, observe State Known becoming false without an OFF
publication.
Observe no immediate TX, then exactly one recovery query at the deterministic
500–1000 ms jitter deadline when RF remains safe and idle.
Confirm that a matching response restores authority, a miss creates no retry,
and a user command or exact OEM activity before the deadline prevents the
timer-expiry query from competing.

### 20.8 Adapter integration gates

On SX127x and SX126x bench units, replay the same logical burst and receive
timeline and verify:

- three identical packet sends with the same inter-frame gaps;
- equivalent physical-completion callbacks and response anchors;
- identical core event/effect traces after normalizing radio diagnostics;
- one and only one RX callback consumer;
- and no family-specific confirmation, retry, or authority policy.

On the Stage-5 SX127x canary, issue a FanCall and verify the entity does not
publish the requested state before a new confirmed-authority revision.
Repeat one failed/missed confirmation and verify the requested state is never
published optimistically.

## 21. Migration path

### 21.1 Migration rule

The two existing configurations remain independently reviewable throughout
migration:

- <code>quietcool-lora32.yaml</code>;
- <code>quietcool-lora-v3.yaml</code>.

Neither is replaced by a generated file during the comparison stages.
The deployed YAML coordinator remains the rollback reference until the C++
canary passes hardware validation.
Display, OLED, buttons, battery, temperatures, and unrelated sensors may stay
YAML-side.
The confirmation machine moves first.

### 21.2 Stage plan

| Stage | Repository result | What is flashed | Main risk and rollback |
|---|---|---|---|
| 0: design | this document only | nothing | design omissions; revise document |
| 1: host core | core classes and host suite, no ESPHome adapter | nothing | semantic mismatch with Rust; host-only correction |
| 2: compile adapter | thin ESPHome adapter and both radio type adapters compile | nothing | ESPHome API mismatch; no device risk |
| 3: receive-only shadow | core receives copied RX/timestamps and logs shadow decisions; YAML remains sole TX/authority owner | one known SX127x canary | loop load or RX forwarding; flash previous YAML binary |
| 4: TX-disabled authority shadow | C++ computes authority and transactions but cannot emit RF; compare against captures | same SX127x canary | diagnostic divergence only; disable shadow |
| 5: single-coordinator C++ canary control | C++ owns TX, confirmation, Refresh guard, and authority; YAML retains UI/display wiring only, with all legacy TX/authority lambdas removed from or compile-time inert in the flashed binary | one controlled SX127x installation only after the structural ownership gate passes | real control regression; flash preserved deployed build |
| 6: SX127x config cutover | <code>quietcool-lora32.yaml</code> becomes config-plus-wiring | SX127x canary, then other verified units one at a time | integration/config regression; retain stage-5 and legacy artifacts |
| 7: SX126x bench | same core through SX126x adapter | bench V3/SX126x board only | unverified pins/radio semantics; do not deploy |
| 8: shared package | common YAML package owns entities/core config; board wrappers own pins/radio | verified units after side-by-side review | packaging/substitution mistakes; wrappers remain small and diffable |
| 9: legacy source cleanup review | remove archived/inert legacy coordinator source from the active configuration tree after parity evidence; it has not been executable in any Stage-5-or-later binary | no special flash | loss of rollback source; preserve tagged release and captures |

### 21.3 Stage gates

Stage 3 requires:

- all host tests passing;
- no core platform includes;
- and an event-log comparator.

Stage 5 requires:

- all historical regression timelines;
- common-path hardware trials;
- forced free-report miss;
- forced watchdog recovery;
- manual Refresh timing;
- and the following machine-verifiable pre-flash ownership checklist, with
  artifacts attached to the canary record:

| Structural assertion over the exact generated Stage-5 build | Required result |
|---|---|
| configured radio <code>on_packet</code> consumer count | exactly 1 |
| sole consumer target | <code>quietcool.on_packet</code> for the C++ controller |
| executable YAML state-command/status-query send actions | 0 |
| executable YAML confirmation, retry, Refresh-authority, and publication lambdas | 0 |
| generated symbols or IDs matching the legacy <code>cl_*</code> coordinator inventory | 0 |
| C++ <code>ConfirmationCore</code> instances owning that radio/sender | exactly 1 |
| FanCall request targets and confirmed-authority publication sources | exactly 1 each, both through <code>QuietCoolComponent</code> |

The check runs against both the resolved YAML/config graph and generated C++,
not source comments or reviewer intent.
Any retained legacy lambda must be excluded by a compile-time false branch and
absent from generated executable callbacks; merely promising not to call it is
not sufficient.
Failure of any count blocks flashing Stage 5.
There is never a flashed binary in which the C++ core and a <code>cl_*</code>
YAML machine can both answer the same HA or radio event.

Stage 7 requires:

- SX126x packet-send completion semantics confirmed;
- FSK configuration confirmed on air;
- RX callback shape confirmed;
- §20.8 parity against the SX127x trace;
- and all timing captures repeated.

No stage authorizes OTA from this design phase.

### 21.4 Side-by-side review artifacts

During stages 3 through 6, retain:

- legacy YAML outcome;
- shadow/core outcome;
- relative event timeline;
- logical TX count;
- packet-send count;
- authority source;
- transaction outcome;
- and any divergence reason.

Comparisons use redacted sender IDs.

### 21.5 Target YAML shape

The target is configuration and wiring, conceptually:

    external_components:
      - source:
          type: local
          path: components
        components:
          - quietcool

    sx127x:
      id: fan_radio
      frequency: 433920000
      modulation: FSK
      bitrate: 2400
      deviation: 10000
      on_packet:
        - quietcool.on_packet:
            id: quietcool_controller
            packet: received_packet

    quietcool:
      id: quietcool_controller
      sender_seed: zero_or_configured_seed
      radio:
        type: sx127x
        id: fan_radio
      command_attempts: 4
      off_command_attempts: 6

    fan:
      - platform: quietcool
        controller_id: quietcool_controller
        name: Whole House Fan

    button:
      - platform: quietcool
        controller_id: quietcool_controller
        kind: refresh
      - platform: quietcool
        controller_id: quietcool_controller
        kind: learn
      - platform: quietcool
        controller_id: quietcool_controller
        kind: forget

The SX126x wrapper changes:

    radio:
      type: sx126x
      id: fan_radio

It does not copy:

- globals;
- lambdas;
- consensus;
- transition guards;
- retry intervals;
- authority publication;
- or learning logic.

The final schema should avoid exposing safety-critical timing constants as
ordinary user substitutions.
They may be compile-time advanced options only with validation and prominent
warnings.

### 21.6 Existing custom fan component

The current <code>quietcool_confirmed_fan</code> platform correctly avoids
optimistic publication, but it delegates behavior into YAML scripts.
Migration should:

1. preserve its confirmation-driven entity semantics;
2. redirect FanCall requests to <code>QuietCoolComponent</code>;
3. publish from <code>AuthoritySnapshot</code>;
4. remove script pointers only after the C++ canary works;
5. and either rename it under the new component or retain a compatibility
   schema for one release.

It must not grow into the new coordinator.

## 22. Open questions and disagreements

### 22.1 The Rust split is semantically proven, not physically ideal

The Rust crate has the right conceptual modules, but its
<code>control.rs</code> is approximately 1,564 lines.
Copying that file shape would violate the explicit maintainability
requirement.
This design ports its behavior while extracting:

- windows/classification;
- transaction budget;
- authority;
- acceptance policy;
- OEM recovery;
- and burst transmission.

That is a deliberate disagreement with a literal source-to-source port.

### 22.2 The “334 core tests” description is imprecise

The Rust workspace lists 334 named tests.
The <code>quietcool-core</code> crate lists 83 named tests.
The implementation plan should use:

- 83 as the direct core parity count;
- 334 as the wider workspace inventory.

Claiming 334 direct core tests would make parity reporting misleading.

### 22.3 Fallback after tail costs miss-path latency

This design waits through +2500 ms classification before the fallback query.
The Rust core currently queues fallback after the +1600 ms acceptance window.
Waiting longer:

- strengthens stale-tail isolation;
- addresses the round-4 reviewer's low-severity tail concern;
- and keeps old candidates out of the new epoch.

It also delays forced-miss confirmation by roughly 900 ms.
This is the most important timing choice for adversarial review.
If reviewers prefer the Rust timing, they must specify another proof that an
old post-command repeat cannot enter fallback acceptance.

### 22.4 Timer anchor is deliberately conservative

Command-burst completion precedes measured report/actuation by under about one
second.
Using it as a timer anchor slightly understates remaining time.
Using query-consensus time after a fallback could overstate remaining time by
much more.
The conservative anchor is safer and below minute-resolution display
precision, but it is not a claim of exact physical timer start.

### 22.5 “Physical control wins” is not fully observable

An exact OEM query is definitive external evidence because the bridge is
half-duplex.
A state frame inside a local response window is physically ambiguous.
The design cannot guarantee perfect attribution.
It uses:

- exact query priority;
- timing correlation;
- consensus;
- prior-authority stale-echo discrimination;
- ON yield;
- and OFF never-yield.

Any design claiming direction from the payload alone is wrong.

### 22.6 State-frame holdoff is conservative

The Rust core grants early-frame external priority only to query epochs.
This design enters explicit holdoff for a strict state in
<code>NoLocalEpoch</code> or a direct-query pre-window to avoid immediate RF
against a possibly missed OEM query.
A strict state in a post-command pre-window is deliberately excluded: it is
logged and ignored so it cannot terminate the transaction or abandon OFF
refires.
Hardware validation should confirm that this does not unnecessarily delay a
legitimate deferred local command.

### 22.7 Radio completion semantics must be proven

The core needs a burst-completion anchor.
If an ESPHome radio API reports queue acceptance rather than on-air
completion, its one <code>send_packet</code> implementation must return the
information needed for the shared BurstTransmitter to apply a conservative
hardware-busy interval.
No second family-specific state machine or callback interface is permitted.
This is a top implementation risk.

### 22.8 Restored state is diagnostic only

Persisting last confirmed state for optimistic UI restoration would improve
appearance after reboot but would violate current-boot authority.
This design refuses optimistic restoration.
If the optional hint adds complexity or write wear, omit it entirely.
Sender ID, seed suppression, and remembered speed are sufficient.

### 22.9 Manual Refresh is refused, not deferred

Deferring Refresh would be safe only with a typed intent and exact later
revalidation.
It offers little value and creates another automatic query source.
This design refuses it outside Idle.
The user can press again after the transaction settles.

### 22.10 Recovery retry is bounded but optional for first release

The Rust reference includes one logical retry after an automatic OEM recovery
query misses.
The deployed YAML has a simpler one-shot behavior.
Keeping the Rust retry preserves proven bounded recovery and remains RF-safe.
A staged first release may disable the retry by configuration only if:

- the enum states remain;
- the configured budget is a typed zero;
- host bounds are updated;
- and no alternate retry loop is added.

This option applies only to the OEM recovery retry.
The estimated-timer-expiry recovery remains exactly one initial query with no
retry and cannot be converted into a configurable loop.

### 22.11 Purpose-parameterized query states were considered and rejected

The review proposed merging the sixteen boot, manual, fallback, and recovery
query-family states into four purpose-parameterized states.
The design keeps all 31 named states.
The merge would make “query pending/transmitting/listening” insufficient to
explain why RF is permitted, which budget owns it, what a miss means, and where
its tail exits.
That recreates the round-4 aliasing shape: “listening” acquires several meanings
that guards must recover from adjacent flags or purpose values.

Duplication is addressed structurally instead:

- query-family contexts share typed payload variants;
- <code>transition_table.cpp</code> expands one lifecycle row template for the
  four named families;
- host tests prove template parity; and
- state IDs remain purpose-specific in traces and RF permission checks.

This is a considered rejection, not an unresolved refactor invitation.

### 22.12 The reducer is table-driven; the line cap is not raised

Three alternatives were considered for the reducer-size conflict:

1. raise <code>confirmation_core.cpp</code>'s cap;
2. split transition ownership at the query-family boundary; or
3. keep one owner and move passive descriptors into a static table module.

The design selects option 3.
It keeps mutation and action dispatch in <code>ConfirmationCore</code>, keeps the
350-line cap, gives passive descriptors the ordinary 400-line source cap, and
uses shared row templates for repeated families.
Raising the cap would defer the maintainability failure; splitting ownership
would weaken global precedence and single-owner review.

### 22.13 Direct-query timing remains start-anchored

The 300 ms direct-query acceptance start overlaps the end of the approximately
400 ms three-frame TX burst.
The anchor remains <code>TxBurstStarted</code> because the measured +417 through
+648 ms reply evidence uses that anchor.
The overlap is safe: the radio is half-duplex, the coordinator remains in a
nonaccepting <code>*QueryTransmitting</code> state, and response acceptance opens
only after <code>TxBurstComplete</code>.
Changing to a burst-end anchor without new end-relative captures could exclude
the earliest measured replies, so §20 requires hardware evidence before such a
change.

## 23. Implementation review gates

An implementation is rejected if any of these is true:

- a core header includes ESPHome, Arduino, or ESP-IDF;
- passive command listening and query listening share an enum value;
- a guard checks a generic active/listening boolean;
- Refresh can schedule outside Idle;
- a refused Refresh mutates authority;
- response-window expiry shares a generic watchdog path;
- a post-command frame before 400 ms can produce ExternalStateHeard, cancel a
  transaction, arm recovery, or spend an allowance;
- PostCommandListening and FallbackResponseListening do not instantiate the
  same exhaustive ordered transaction-consensus row template;
- a live transaction consensus can reach the invalid-internal-event default;
- timer remaining authority gates fan-state promotion;
- OFF can yield to a state-frame mismatch;
- an OFF-request policy return type contains a yield alternative;
- a local tail contradiction rewrites a typed tail exit or arms recovery;
- any state other than PostCommandTailWait can create FallbackQueryPending;
- estimated local timer expiry can leave Idle without one bounded jittered
  observation allowance, or that allowance can retry/renew;
- an automatic retry counter exists outside its named owner;
- a radio adapter contains burst or retry policy;
- <code>confirmation_core.cpp</code> exceeds 350 lines;
- transition descriptors can mutate core state, or
  <code>transition_table.cpp</code> exceeds 400 lines;
- any production source exceeds 400 lines without design revision;
- current state is restored as authoritative after reboot;
- external exact state is published directly as confirmed;
- estimated timer expiry publishes OFF;
- SX127x and SX126x compile different core policy;
- a Stage-5 generated binary has other than one radio <code>on_packet</code>
  consumer, contains executable legacy TX/authority lambdas, or contains a
  competing <code>cl_*</code> coordinator;
- §20.2 has not collected more than seven new thermal-condition trials before
  locking <code>kPostCommandAcceptStartMs</code>;
- §20.8 adapter integration gates have not passed;
- or host tests require ESPHome or hardware.

## 24. Design summary

### Module list

Core behavior classes:

1. SenderId
2. FanState
3. FrameCodec
4. FrameRecovery
5. ConsensusTracker
6. ResponseWindow
7. ResponseClassifier
8. CommandTransaction
9. AuthorityStore
10. ObservationPolicy
11. RecoveryScheduler
12. LearnMachine
13. ConfirmationCore

Passive core data module:

14. TransitionTable

Ports and integration classes:

15. Clock
16. Radio
17. EventSink
18. BurstTransmitter
19. Sx127xRadioAdapter
20. Sx126xRadioAdapter
21. QuietCoolComponent
22. QuietCoolFan

### State count

The coordinator has 31 explicit states.
Authority, response epochs, tail exits, TX progress, recovery phases, and
learning context use separate explicit sum types.
Recovery cause is also typed, allowing OEM and one-shot timer-expiry recovery
to share the named recovery states without sharing budgets.

### Invariant list

The design defines 25 stable invariant IDs: INV-01 through INV-23 are direct
host invariants, INV-24 is a hardware/integration gate, and INV-25 is a hybrid
host/integration gate.

- OFF never yields;
- OFF has five fully spendable re-fires;
- ON has a fixed budget;
- exact OEM query wins;
- unprovisioned is RF silent;
- one burst at a time;
- command RF is bounded;
- OEM recovery RF is bounded;
- Refresh is idle-only;
- no pre-actuation query;
- one fallback per missed free report;
- no consensus leakage;
- classification is not acceptance;
- externally prioritized commands are not confirmation, while post-command
  pre-acceptance states are ignored;
- timer authority cannot block state authority;
- expiry never confirms OFF and schedules at most one bounded observation;
- future ON work cannot publish OFF;
- stalls remain recoverable;
- only started commands spend attempts;
- stale callbacks are inert;
- time cannot move backward;
- learning is exclusive and receive-only;
- persistence cannot resurrect RF;
- radio families are behaviorally identical;
- and publication is confirmation-driven.

### Rust reference versus fresh design

Planned semantic ports from Rust:

- validated domain types;
- frame encode/decode;
- bounded response recovery;
- consensus thresholds and 60 ms independence;
- two-press learning boundaries;
- fixed 4/6 command attempt budgets;
- semantic duplicate join and supersession;
- prior-authority stale-echo policy;
- OFF re-aim and never-yield;
- exact OEM query priority;
- state-versus-timer authority behavior;
- bounded OEM recovery;
- tokenized TX completion;
- and the core test scenario inventory.

Designed fresh for this C++/ESPHome migration:

- the 31-state decomposition;
- separate post-command and query state families;
- ResponseWindow and ResponseClassifier extraction;
- ObservationPolicy extraction;
- AuthorityStore as sole authority writer;
- a passive table-driven reducer with shared query and consensus row templates;
- tail-exit variants;
- explicit radio-recovery states;
- response-deadline priority over watchdog;
- fallback after tail quarantine;
- Clock/Radio/EventSink ports;
- shared BurstTransmitter;
- two one-method radio adapters;
- versioned restorable-state effects;
- thin component/entity boundaries;
- and the staged YAML migration.

### Top three implementation risks

1. Radio completion semantics: the response anchor must represent the third
   packet's physical completion consistently on SX127x and SX126x.
2. Physical timing versus conservative tail policy: waiting until +2500 ms is
   safer for stale repeats but needs miss-path latency validation.
3. Adapter thinness under ESPHome constraints: code generation, RX callback
   wiring, persistence, and entity publication must not pull policy back out
   of the core.

## 25. Phase boundary

This design phase creates only:

- <code>docs/claude/cpp-core-design.md</code>.

It creates no:

- C++ source;
- buildable header;
- CMake file;
- test scaffold;
- ESPHome config change;
- secret;
- sender ID;
- commit;
- flash;
- upload;
- or OTA action.

Implementation begins only after adversarial design review and explicit user
authorization.
