# Timer control and restored diagnostics — design

Date: 2026-07-28
Status: implemented on `feat/timer-control-and-diagnostics`; awaiting adversarial
review and hardware verification. §3.5 carries a correction found during
implementation.
Scope: `components/quietcool/esphome/`, `quietcool-cpp-lora32.yaml`, tests

The C++ component replaced the legacy YAML build in every respect except one
capability: the YAML build could **send** timer commands and the C++ build
cannot. It reports a countdown it has no way to start. This restores that, and
restores the eight diagnostic entities the migration dropped, as
disabled-by-default entities.

Nothing in `components/quietcool/core/` changes. The core already models
durations, already builds timer command bytes, and already anchors and recovers
timers. Only the entity layer was ever missing.

---

## 1. The duration nibble, and why `Off` and `Continuous` are not redundant

**This is the least intuitive part of the protocol. Read this section before
touching any duration code.**

A command byte is one byte, three fields (`fan_state.cpp:19`):

```
  bit   7 6   5 4   3 2 1 0
        1 0   spd   duration
        └─┬┘  └┬┘   └───┬──┘
    marker   1..3    0,1,2,4,8,12,15
```

- **bits 7-6** — `10` on any byte we transmit (the outbound command marker).
  On a frame *received* from a fan the same two bits carry the fan's speed
  capability, which is why an outbound `10` aliases `SpeedCapability::Two` and
  why the echo guard exists.
- **bits 5-4** — speed: `1`=LOW, `2`=MED, `3`=HIGH. These meanings are **fixed
  on the wire**, and a given fan supports a *subset* of them (issue #30).
- **bits 3-0** — duration.

Worked examples: HIGH + continuous = `0b10_11_1111` = `0xBF`. HIGH + 1 hour =
`0b10_11_0001` = `0xB1`. LOW + 2 hours = `0b10_01_0010` = `0x92`.

### The trap

| Duration | Nibble | Fan is | Timer |
|---|---|---|---|
| `Off` | `0x0` | **stopped** | n/a |
| `Hours1/2/4/8/12` | `0x1/2/4/8/C` | **running** | counting down |
| `Continuous` | `0xF` | **running** | none |

`Off` and `Continuous` both describe a fan with no timer counting down, which
is why they read as redundant. They are opposites:

> **`Off` means the fan is not running. `Continuous` means the fan is running
> and will not stop on its own.**

The rest of the codebase already depends on exactly this. `fan_state.h:34`:

```cpp
bool is_on() const { return duration() != Duration::Off; }
```

Every duration except `Off` means *running*. And the fan entity
(`fan_command.cpp:46`) is built on it:

```cpp
const auto duration = on ? Duration::Continuous : Duration::Off;
```

Turning the fan **on** transmits `speed|0xF`; turning it **off** transmits
`speed|0x0`.

**Consequence if collapsed:** treating the two as interchangeable makes a
"clear the timer" action transmit `speed|0x0` and *stop the fan*. That is the
same shape of defect as issue #30 — a wrong nibble that stops a fan the user
asked to run — arriving through a different door. Any future refactor that
notices the apparent redundancy must read this section first.

### Consequences for this design

- The timer select never offers `Off`. Stopping the fan belongs to the fan
  entity. The select chooses only among **running** durations.
- The select's `None` option means *no timer*, i.e. `Continuous`, i.e. the fan
  runs until stopped. It transmits `speed|0xF`.

---

## 2. Decisions taken (and their costs)

Recorded because each was a fork with a real downside, chosen deliberately.

**D1 — A timer command from OFF starts the fan.** Matching the legacy build:
the speed nibble is the fan's current speed, defaulting to LOW when the fan is
off. Selecting "2 hours" on a stopped fan transmits `0x92` and the fan starts.

*Cost:* a dropdown in Home Assistant becomes an action that energizes the fan.
See §6.

**D2 — `None` genuinely clears the timer.** It transmits `speed|Continuous`.
The protocol has no non-actuating "clear timer" command, so on a fan whose
timer already expired and which has therefore stopped, `None` **restarts it**.

*Cost:* the option that reads most like a no-op is an energizing command. The
legacy build avoided this by making `None` refuse itself with a warning, which
traded the hazard for an option that never did anything.

**D3 — The entity stays named `Fan Timer` and the option stays named `None`.**
Considered and rejected: renaming to "Fan Run Duration" (which would make
`Continuous` a coherent answer), and labelling the option "No timer (runs
continuously)".

*Cost:* "None" understates that it transmits. Mitigated by entity description
text and by this document, not by the label.

**D4 — Diagnostics return disabled by default.** All eight, with
`entity_category: diagnostic` and `disabled_by_default: true`, so they cost
nothing until someone opts in.

---

## 3. Components

### 3.1 `esphome/timer_command.{h,cpp}` — new, pure

Mirrors the existing `fan_command.cpp`: no ESPHome dependencies, host-testable
in isolation.

```cpp
// Off is deliberately unrepresentable here: this function never stops a fan.
enum class TimerSelection : std::uint8_t {
  Continuous, Hours1, Hours2, Hours4, Hours8, Hours12
};

::quietcool::FanState timer_command_from_intent(
    TimerSelection requested,
    bool fan_on,                       // current confirmed run state
    int level,                         // current confirmed HA level
    std::uint8_t command_speed_count); // the COMMAND band, not the entity band
```

Speed selection is `fan_on ? speed_for_level(level, command_speed_count)
: Speed::Low`, then `FanState::command(speed, requested)`.

**It reuses `speed_for_level` rather than casting.** A timer command carries a
speed nibble like any other command, so a 2-speed fan must map positionally
here too. Skipping that would let a timer transmit MED to a fan that has no
MED — reintroducing issue #30 through the timer path. It takes the **command**
band (`command_speed_count`), not the entity band, for the reason in issue #31:
while capability is unknown the command band structurally cannot form MED.

A dedicated `TimerSelection` enum, rather than reusing `Duration`, makes
"stop the fan" **unrepresentable** in the timer path instead of merely
forbidden by a comment. Given §1, a `Duration` parameter would accept `Off`
from any future caller and silently turn a timer request into a stop command.
The mapping to `Duration` happens inside the function, where §1's table is the
only place the two vocabularies meet.

### 3.2 `QuietCoolTimerSelect` — new entity

An ESPHome `select` plus its Python codegen. Name `Fan Timer`; options
`None`, `1 hour`, `2 hours`, `4 hours`, `8 hours`, `12 hours`.

`control()` maps the option to a `Duration` (`None` → `Continuous`), reads the
fan's current confirmed on/level, and calls
`QuietCoolComponent::request_state()`. It is **not optimistic**: it publishes
only what confirmed authority reports, exactly like the fan entity. A refusal
(unprovisioned, busy, learning) leaves the published option unchanged.

### 3.3 Authority fan-out — small change to `QuietCoolComponent`

`set_authority_publisher()` currently stores a single `AuthorityPublisher*`
(the fan). It becomes `add_authority_publisher()` backed by a fixed-capacity
array — no heap, following the `CoreEffects::kCapacity` pattern — so the select
and the diagnostics receive the same snapshots. Overflow is a compile-time
capacity choice, not a runtime failure path.

This does not weaken the confirmation rule: every publisher still receives only
`AuthoritySnapshot`s, which only confirmed evidence produces.

### 3.4 Select feedback mapping

From `AuthoritySnapshot::timer`:

| Variant | Published option |
|---|---|
| `LocallyAnchoredTimerAuthority` | its `Duration` |
| `ProgrammedDurationAuthority` | its `Duration` |
| `NoActiveTimerAuthority` | `None` |
| `UnknownTimerAuthority` | nothing published (last value retained) |

### 3.5 Diagnostics — eight entities

Text sensors, from state the component already holds:

| Entity | Source |
|---|---|
| `Last TX Command` | in-flight outbound byte (already tracked for the echo guard), hex |
| `Last Valid RX Frame` | last accepted decoded frame, hex |
| `Last Confirmed Fan State` | `AuthoritySnapshot::state` |
| `Fan Speed Capability` | `AuthoritySnapshot::speed_capability` |
| `Remote Sender ID` | provisioning record |

Numeric sensors, requiring **new monotonic counters** on `QuietCoolComponent`,
incremented at the radio boundary:

| Entity | Incremented when |
|---|---|
| `TX Count` | a burst transmission completes |
| `RX Valid Count` | a received frame passes `FrameCodec::decode_strict` |
| `RX Rejected Count` | a received frame fails it, or arrives unprovisioned |

> **Corrected during implementation.** This section first defined the RX
> counters as "accepted" versus "rejected" in the classifier's sense. That is
> not implementable from the adapter and would have been wrong anyway.
> `on_radio_packet` has no accept/reject branch — the relevance decision lives
> inside `ConfirmationCore::on_frame`, which this work may not modify — and the
> obvious proxy is a trap: `on_frame` returns empty `CoreEffects` for a
> genuinely accepted frame whenever consensus is not yet reached, which is the
> *common* case, since consensus needs 2-3 independent candidates. Treating
> emptiness as rejection would have systematically miscounted normal traffic.
>
> The legacy build settles the semantics: it incremented its rejected counter at
> frame-**validation** failures (`legacy/quietcool-lora32.yaml` lines 655, 713,
> 1017, 1022, 1027, 1032 — length, tail, sender mismatch, invalid state) and its
> valid counter at 984 and 1054. Those map exactly onto `FrameDecodeError`. So
> the counters key on `FrameCodec::decode_strict`, a static pure function the
> adapter may *call* without modifying core, against a cached
> `provisioned_sender_`. The core decodes again; that duplicate decode is
> deliberate and inert — do not "optimize" it by reaching into core.

`TX Count` is called out specifically: a flat TX Count through the OEM remote's
retry storm is the evidence that exonerated the bridge of jamming the remote.
Losing it removed the instrument that settled that question.

---

## 4. YAML binding — a first-class requirement

Every new entity must be declared in `quietcool-cpp-lora32.yaml`, and the
config regression suite must assert that each one is declared.

This is not boilerplate. Issue #9's `Controller Fault` sensor was implemented,
unit-tested, adversarially reviewed and attested — and never reached the
hardware, because no YAML line declared it (issue #28). Implementation without
binding is indistinguishable from no implementation, so the binding gets a test
of its own.

---

## 5. Testing

- **Host tests** for `timer_command_from_intent`: each duration; fan-on uses
  the current level; fan-off defaults to LOW; the 2-speed positional case
  (level 2 of 2 → HIGH + duration, never MED); `Duration::Off` rejected.
- **Adapter tests** against the ESPHome stubs: option→duration mapping,
  confirmed-only publication, each timer authority variant, refusal leaves the
  published option unchanged, counters increment on the right events.
- **Config regression tests**: every new entity is declared in the shipped
  YAML.
- **Mutation testing on every new assertion.** A green host suite has been
  misleading twice in this project. Each new test must be shown to fail when
  the property it claims to protect is broken; `rm -rf tests/cpp/build` before
  every run, and empty output means "did not compile", never "passed".

---

## 6. Safety and rollout

Every option in the timer select transmits an **energizing** command, `None`
included (D1, D2). Setting a timer on a stopped fan starts it; `None` on a fan
whose timer expired restarts it.

This drives a whole-house fan, which pulls air through the house and can
backdraft combustion appliances. Therefore:

- Host tests, compilation and config validation are unrestricted.
- The standing rule applies: a production RF flash is gated on a clean
  adversarial review from the four-engine roster, not on tests plus compile.
- **No timer command is sent on-air until the maintainer confirms a window is
  open.** Flashing itself and the boot status query are non-energizing and do
  not require one.

Build with `esphome-config/.venv/bin/esphome` (ESP-IDF, `loop_task_stack_size:
16384`); confirm both in `esphome config` output before compiling.
