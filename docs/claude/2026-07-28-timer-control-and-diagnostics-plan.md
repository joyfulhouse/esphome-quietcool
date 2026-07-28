# Timer Control and Restored Diagnostics — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the C++ component the ability to *send* timer commands (1/2/4/8/12 hours and continuous), and return the eight diagnostic entities the YAML→C++ migration dropped, disabled by default.

**Architecture:** Entity layer only. `components/quietcool/core/` is not modified — `Duration`, `FanState::command(speed, duration)`, timer anchoring and timer-expiry recovery already exist. New work is a pure mapping unit, a `select` entity, an authority fan-out change, three counters, and YAML bindings with tests that assert those bindings exist.

**Tech Stack:** C++17, ESPHome external component (ESP-IDF), host test suites via `make -C tests/cpp`, Python codegen (`components/quietcool/*.py`), config regression tests in `tests/test_quietcool_esphome_config.py` (stdlib `unittest`).

**Spec:** `docs/claude/2026-07-28-timer-control-and-diagnostics-design.md` — read §1 before writing any duration code.

## Global Constraints

- **`Duration::Off` (`0x0`) means the fan is STOPPED. `Duration::Continuous` (`0xF`) means the fan RUNS with no timer.** They are opposites, not duplicates. See spec §1.
- The timer path must never be able to express `Off`. Use the `TimerSelection` enum, not `Duration`, in any entity-facing signature.
- Timer commands carry a speed nibble, so they must map speed **positionally** via `speed_for_level()` (issue #30), against the **command** band, not the entity band (issue #31).
- Entities publish **only confirmed authority**. Nothing optimistic.
- All eight diagnostics: `entity_category: diagnostic` and `disabled_by_default: true`.
- Every new entity must be declared in `quietcool-cpp-lora32.yaml` **and** asserted by a config test (issue #28: implemented, tested, reviewed, never bound).
- `tests/cpp/Makefile` `ADAPTER_SOURCES` filters out `.cpp` files deriving from ESPHome entity base classes. Any new entity `.cpp` must be added to that filter, and therefore all its logic must live in pure functions that *are* linked.
- Mutation-test every new assertion: `rm -rf tests/cpp/build` before each run; empty output means "did not compile", never "passed".
- **No timer command may be transmitted on-air until the maintainer confirms an open window.** Compiling and host tests are unrestricted.
- Build with `/Users/bryanli/Projects/joyfulhouse/esphome-config/.venv/bin/esphome` (2026.7.2); config must still show `type: esp-idf` and `loop_task_stack_size: 16384`.

---

### Task 1: Pure timer command mapping

**Files:**
- Create: `components/quietcool/esphome/timer_command.h`
- Create: `components/quietcool/esphome/timer_command.cpp`
- Test: `tests/cpp/adapter/timer_command_test.cpp`

**Interfaces:**
- Consumes: `speed_for_level(int, uint8_t)` and `clamp_fan_speed` from `esphome/fan_command.h`; `FanState::command`, `Speed`, `Duration` from `core/fan_state.h`.
- Produces:
  - `enum class TimerSelection : std::uint8_t { Continuous, Hours1, Hours2, Hours4, Hours8, Hours12 }`
  - `::quietcool::Duration duration_for_selection(TimerSelection)`
  - `::quietcool::FanState timer_command_from_intent(TimerSelection requested, bool fan_on, int level, std::uint8_t command_speed_count)`

`timer_command.cpp` is picked up automatically by the `ADAPTER_SOURCES` wildcard — no Makefile change in this task.

- [ ] **Step 1: Write the failing test**

Create `tests/cpp/adapter/timer_command_test.cpp`:

```cpp
// Timer actuation mapping: TimerSelection -> FanState command byte.
//
// A timer command is speed|duration in ONE byte, so it is an energizing
// command and it carries a speed nibble like any other. Two failure modes are
// guarded here: expressing Duration::Off (which would STOP a fan the user
// asked to run for N hours), and mapping speed by identity (which on a
// 2-speed fan transmits MED, a speed it does not have — issue #30).

#include "quietcool/esphome/timer_command.h"

#include "quietcool/core/fan_state.h"

#include "support/test.h"

#include <cstdint>

namespace esphome::quietcool {
namespace {

using ::quietcool::Duration;
using ::quietcool::FanState;
using ::quietcool::Speed;

constexpr std::uint8_t kTwoSpeeds = 2;
constexpr std::uint8_t kThreeSpeeds = 3;

TEST(timer_selection_maps_to_its_duration) {
  EXPECT_EQ(duration_for_selection(TimerSelection::Continuous), Duration::Continuous);
  EXPECT_EQ(duration_for_selection(TimerSelection::Hours1), Duration::Hours1);
  EXPECT_EQ(duration_for_selection(TimerSelection::Hours2), Duration::Hours2);
  EXPECT_EQ(duration_for_selection(TimerSelection::Hours4), Duration::Hours4);
  EXPECT_EQ(duration_for_selection(TimerSelection::Hours8), Duration::Hours8);
  EXPECT_EQ(duration_for_selection(TimerSelection::Hours12), Duration::Hours12);
}

TEST(no_selection_can_produce_a_stop_command) {
  // Every selection must leave the fan running. Duration::Off is the stop
  // command; it must be unreachable from the timer path.
  const TimerSelection all[] = {
      TimerSelection::Continuous, TimerSelection::Hours1, TimerSelection::Hours2,
      TimerSelection::Hours4,     TimerSelection::Hours8, TimerSelection::Hours12};
  for (const auto selection : all) {
    const auto command = timer_command_from_intent(selection, true, 1, kThreeSpeeds);
    EXPECT_TRUE(command.is_on());
    EXPECT_TRUE(command.duration() != Duration::Off);
  }
}

TEST(running_fan_keeps_its_speed_and_takes_the_duration) {
  // High + 1 hour == 0xB1, per the legacy byte table.
  const auto command = timer_command_from_intent(TimerSelection::Hours1, true, 3, kThreeSpeeds);
  EXPECT_EQ(command.outbound_command_byte(), 0xB1);
  EXPECT_EQ(command.speed().value(), Speed::High);
  EXPECT_EQ(command.duration(), Duration::Hours1);
}

TEST(stopped_fan_defaults_to_low_and_starts) {
  // Legacy behavior, deliberately retained: a timer set on a stopped fan
  // STARTS it at LOW. Low + 2 hours == 0x92.
  const auto command = timer_command_from_intent(TimerSelection::Hours2, false, 3, kThreeSpeeds);
  EXPECT_EQ(command.outbound_command_byte(), 0x92);
  EXPECT_EQ(command.speed().value(), Speed::Low);
  EXPECT_TRUE(command.is_on());
}

TEST(two_speed_top_level_takes_high_never_medium) {
  // The band is positional: level 2 of 2 is HIGH (0xB_), never MED (0xA_).
  // An identity cast here re-creates issue #30 through the timer path.
  const auto command = timer_command_from_intent(TimerSelection::Hours4, true, 2, kTwoSpeeds);
  EXPECT_EQ(command.speed().value(), Speed::High);
  EXPECT_EQ(command.outbound_command_byte(), 0xB4);
}

TEST(continuous_selection_matches_the_fan_entitys_on_command) {
  // Selecting "None" (Continuous) must produce exactly what turning the fan on
  // produces, or the two controls would disagree about what "running" means.
  const auto command = timer_command_from_intent(TimerSelection::Continuous, true, 3, kThreeSpeeds);
  EXPECT_EQ(command.outbound_command_byte(), 0xBF);
}

}  // namespace
}  // namespace esphome::quietcool
```

- [ ] **Step 2: Run the test to verify it fails**

Run:
```bash
cd /Users/bryanli/Projects/joyfulhouse/esphome-quietcool
rm -rf tests/cpp/build && make -C tests/cpp test-adapter
```
Expected: compile failure — `quietcool/esphome/timer_command.h: No such file or directory`.

- [ ] **Step 3: Write the header**

Create `components/quietcool/esphome/timer_command.h`:

```cpp
#pragma once

#include "quietcool/core/fan_state.h"

#include <cstdint>

namespace esphome::quietcool {

// The durations a user may select for the fan to RUN. Deliberately not
// ::quietcool::Duration: that enum includes Off, which STOPS the fan (see
// Duration's own comment and the design doc §1). Reusing it here would let a
// future caller turn a timer request into a stop command — issue #30's failure
// shape by another route. Stopping the fan belongs to the fan entity alone.
enum class TimerSelection : std::uint8_t {
  Continuous, Hours1, Hours2, Hours4, Hours8, Hours12
};

// Maps a selection onto its wire duration nibble. Total; never yields Off.
::quietcool::Duration duration_for_selection(TimerSelection selection);

// Translates a timer selection into the FanState command driven onto the RF
// link. A timer command is speed|duration in one byte, so it is ENERGIZING: on
// a stopped fan it starts it, at LOW, matching the legacy YAML build. The speed
// nibble is mapped POSITIONALLY through speed_for_level(), against the COMMAND
// band — a 2-speed fan's top level is HIGH (0xB_), never MED (0xA_).
::quietcool::FanState timer_command_from_intent(TimerSelection requested, bool fan_on,
                                                int level,
                                                std::uint8_t command_speed_count);

}  // namespace esphome::quietcool
```

- [ ] **Step 4: Write the implementation**

Create `components/quietcool/esphome/timer_command.cpp`:

```cpp
#include "timer_command.h"

#include "fan_command.h"

namespace esphome::quietcool {

::quietcool::Duration duration_for_selection(TimerSelection selection) {
  switch (selection) {
    case TimerSelection::Hours1:  return ::quietcool::Duration::Hours1;
    case TimerSelection::Hours2:  return ::quietcool::Duration::Hours2;
    case TimerSelection::Hours4:  return ::quietcool::Duration::Hours4;
    case TimerSelection::Hours8:  return ::quietcool::Duration::Hours8;
    case TimerSelection::Hours12: return ::quietcool::Duration::Hours12;
    case TimerSelection::Continuous: break;
  }
  // Continuous is the default rather than a case so that adding a selection
  // without extending this switch keeps the fan RUNNING. Failing toward
  // Continuous is the safe direction; failing toward Off would stop the fan.
  return ::quietcool::Duration::Continuous;
}

::quietcool::FanState timer_command_from_intent(TimerSelection requested, bool fan_on,
                                                int level,
                                                std::uint8_t command_speed_count) {
  const auto speed = fan_on ? speed_for_level(level, command_speed_count)
                            : ::quietcool::Speed::Low;
  return ::quietcool::FanState::command(speed, duration_for_selection(requested));
}

}  // namespace esphome::quietcool
```

- [ ] **Step 5: Run the tests to verify they pass**

Run:
```bash
rm -rf tests/cpp/build && make -C tests/cpp test-adapter
```
Expected: PASS, adapter test count increased by 6.

- [ ] **Step 6: Mutation-test the two safety claims**

Make each mutation, run `rm -rf tests/cpp/build && make -C tests/cpp test-adapter`, confirm the named test FAILS, then revert:

1. In `timer_command_from_intent`, replace `speed_for_level(level, command_speed_count)` with `static_cast<::quietcool::Speed>(level)`.
   Expected: `two_speed_top_level_takes_high_never_medium` FAILS.
2. In `duration_for_selection`, change the `Continuous` fallthrough to `return ::quietcool::Duration::Off;`.
   Expected: `no_selection_can_produce_a_stop_command` and `continuous_selection_matches_the_fan_entitys_on_command` FAIL.

If a mutation produces empty output, it did not compile — that is not a pass. Fix and re-run.

- [ ] **Step 7: Commit**

```bash
git add components/quietcool/esphome/timer_command.h \
        components/quietcool/esphome/timer_command.cpp \
        tests/cpp/adapter/timer_command_test.cpp
git commit -m "feat: pure timer command mapping, with Off unrepresentable"
```

---

### Task 2: Authority fan-out to multiple publishers

**Files:**
- Modify: `components/quietcool/esphome/quietcool_component.h` (`set_authority_publisher`, publisher member)
- Modify: `components/quietcool/esphome/quietcool_component.cpp` (publication site)
- Modify: `components/quietcool/esphome/quietcool_fan.h:14-17` (`set_controller`)
- Test: `tests/cpp/adapter/authority_fanout_test.cpp`

**Interfaces:**
- Produces: `void QuietCoolComponent::add_authority_publisher(AuthorityPublisher*)`, replacing `set_authority_publisher`. Capacity constant `QuietCoolComponent::kMaxAuthorityPublishers = 4`.

Why: the select and the diagnostics need the same confirmed snapshots the fan gets, and the component currently stores exactly one publisher pointer.

- [ ] **Step 1: Write the failing test**

Create `tests/cpp/adapter/authority_fanout_test.cpp`:

```cpp
// Every registered publisher must receive every authority snapshot. Before
// this, the component held ONE publisher pointer, so registering a second
// entity silently replaced the first — the fan would have stopped updating the
// moment the timer select was added.

#include "quietcool/esphome/quietcool_component.h"

#include "support/test.h"

#include <vector>

namespace esphome::quietcool {
namespace {

class CountingPublisher final : public AuthorityPublisher {
 public:
  void publish_authority(const ::quietcool::AuthoritySnapshot&) override { ++calls; }
  int calls{0};
};

TEST(every_registered_publisher_receives_a_snapshot) {
  CountingPublisher first;
  CountingPublisher second;
  auto component = make_test_component();  // support helper, see Step 3
  component->add_authority_publisher(&first);
  component->add_authority_publisher(&second);

  component->publish_authority_for_test();

  EXPECT_EQ(first.calls, 1);
  EXPECT_EQ(second.calls, 1);
}

TEST(registering_past_capacity_does_not_displace_an_existing_publisher) {
  CountingPublisher publishers[QuietCoolComponent::kMaxAuthorityPublishers + 1];
  auto component = make_test_component();
  for (auto& publisher : publishers) component->add_authority_publisher(&publisher);

  component->publish_authority_for_test();

  // The first kMax are kept; the overflow one is dropped, never swapped in.
  EXPECT_EQ(publishers[0].calls, 1);
  EXPECT_EQ(publishers[QuietCoolComponent::kMaxAuthorityPublishers].calls, 0);
}

}  // namespace
}  // namespace esphome::quietcool
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `rm -rf tests/cpp/build && make -C tests/cpp test-adapter`
Expected: compile failure — no member `add_authority_publisher`.

- [ ] **Step 3: Implement the fan-out**

In `quietcool_component.h`, replace the single-publisher setter:

```cpp
  static constexpr std::size_t kMaxAuthorityPublishers = 4;

  // Fixed capacity, no heap — same discipline as CoreEffects::kCapacity. Four
  // covers the fan, the timer select and headroom; an overflow registration is
  // DROPPED rather than displacing a live publisher, because silently
  // unsubscribing the fan entity would strand Home Assistant on stale state.
  void add_authority_publisher(AuthorityPublisher* publisher) {
    if (publisher == nullptr) return;
    if (authority_publisher_count_ >= kMaxAuthorityPublishers) return;
    authority_publishers_[authority_publisher_count_++] = publisher;
  }
```

with members:

```cpp
  AuthorityPublisher* authority_publishers_[kMaxAuthorityPublishers]{};
  std::size_t authority_publisher_count_{0};
```

Add a test hook alongside the existing `*_for_test` methods:

```cpp
  void publish_authority_for_test() { publish_authority_to_all(store_snapshot_now()); }
```

In `quietcool_component.cpp`, change the publication site from the single
pointer to a loop over `authority_publishers_[0 .. authority_publisher_count_)`.

In `quietcool_fan.h`, change `set_controller` to call
`controller_->add_authority_publisher(this);`.

Add `make_test_component()` to `tests/cpp/support/` if no equivalent helper
exists; follow the construction used by `tests/cpp/adapter/component_deferral_test.cpp`.

- [ ] **Step 4: Run the tests to verify they pass**

Run: `rm -rf tests/cpp/build && make -C tests/cpp test-adapter && make -C tests/cpp test`
Expected: PASS, and no existing test regresses.

- [ ] **Step 5: Mutation-test the capacity guard**

Change the overflow branch to overwrite slot 0 instead of returning. Expected:
`registering_past_capacity_does_not_displace_an_existing_publisher` FAILS. Revert.

- [ ] **Step 6: Commit**

```bash
git add components/quietcool/esphome/quietcool_component.h \
        components/quietcool/esphome/quietcool_component.cpp \
        components/quietcool/esphome/quietcool_fan.h \
        tests/cpp/adapter/authority_fanout_test.cpp tests/cpp/support
git commit -m "feat: fan out authority snapshots to multiple publishers"
```

---

### Task 3: Radio counters for the numeric diagnostics

**Files:**
- Modify: `components/quietcool/esphome/quietcool_component.h` (counters, getters, sensor setters)
- Modify: `components/quietcool/esphome/quietcool_component.cpp` (increment sites, publication)
- Test: `tests/cpp/adapter/radio_counters_test.cpp`

**Interfaces:**
- Produces: `std::uint32_t tx_count() const`, `rx_valid_count() const`, `rx_rejected_count() const`; setters `set_tx_count_sensor`, `set_rx_valid_count_sensor`, `set_rx_rejected_count_sensor`.

`TX Count` specifically: a flat TX Count through the OEM remote's retry storm is
the evidence that exonerated this bridge of jamming the remote. Its absence
removed the instrument that settled that question.

- [ ] **Step 1: Write the failing test**

Create `tests/cpp/adapter/radio_counters_test.cpp` with three tests:
`tx_count_increments_once_per_completed_burst`,
`rx_valid_count_increments_on_an_accepted_frame`,
`rx_rejected_count_increments_on_a_rejected_frame`. Drive them through the
component's existing radio entry points (`on_radio_packet` with a well-formed
frame for the accepted case, and with a malformed frame for the rejected case);
follow the frame construction already used in
`tests/cpp/adapter/component_deferral_test.cpp`.

- [ ] **Step 2: Run to verify it fails**

Run: `rm -rf tests/cpp/build && make -C tests/cpp test-adapter`
Expected: compile failure — no member `tx_count`.

- [ ] **Step 3: Implement the counters**

Add to `quietcool_component.h`:

```cpp
  std::uint32_t tx_count() const { return tx_count_; }
  std::uint32_t rx_valid_count() const { return rx_valid_count_; }
  std::uint32_t rx_rejected_count() const { return rx_rejected_count_; }
```

with `std::uint32_t tx_count_{0}, rx_valid_count_{0}, rx_rejected_count_{0};`.
Increment `rx_valid_count_` / `rx_rejected_count_` at the accept/reject branches
of `on_radio_packet`, and `tx_count_` where a burst completion is handled in
`apply_burst_event`. Counters are monotonic and never reset outside a reboot.

- [ ] **Step 4: Run to verify they pass**

Run: `rm -rf tests/cpp/build && make -C tests/cpp test-adapter`
Expected: PASS.

- [ ] **Step 5: Mutation-test**

Swap the increments of `rx_valid_count_` and `rx_rejected_count_`. Expected:
both RX tests FAIL. Revert.

- [ ] **Step 6: Commit**

```bash
git add components/quietcool/esphome/quietcool_component.h \
        components/quietcool/esphome/quietcool_component.cpp \
        tests/cpp/adapter/radio_counters_test.cpp
git commit -m "feat: monotonic TX/RX counters for the restored diagnostics"
```

---

### Task 4: Diagnostic publication and codegen kinds

**Files:**
- Modify: `components/quietcool/esphome/quietcool_component.h` / `.cpp` (five text setters + publication)
- Modify: `components/quietcool/text_sensor.py` (five new kinds)
- Modify: `components/quietcool/sensor.py` (three counter kinds, distinct schema)
- Test: `tests/cpp/adapter/diagnostic_publication_test.cpp`

**Interfaces:**
- Produces setters: `set_last_tx_command_sensor`, `set_last_rx_frame_sensor`, `set_last_confirmed_state_sensor`, `set_speed_capability_sensor`, `set_remote_sender_id_sensor`, plus the three counter setters from Task 3.
- Produces text formats: `Last TX Command` and `Last Valid RX Frame` as `"0xB1"`-style uppercase hex; `Fan Speed Capability` as `"unknown" | "1" | "2" | "3"`; `Remote Sender ID` as `"0xCB004739"`; `Last Confirmed Fan State` as `"off" | "low" | "medium" | "high"` suffixed with the duration when a timer is programmed, e.g. `"high 1h"`.

- [ ] **Step 1: Write the failing test**

Create `tests/cpp/adapter/diagnostic_publication_test.cpp` asserting each
format above, including that `Fan Speed Capability` publishes `"unknown"`
(never a fabricated `3`) when the snapshot's `speed_capability` is empty.

- [ ] **Step 2: Run to verify it fails**

Run: `rm -rf tests/cpp/build && make -C tests/cpp test-adapter`
Expected: compile failure — no member `set_last_tx_command_sensor`.

- [ ] **Step 3: Implement publication**

Add the five text-sensor pointers and setters; publish them from the same place
the existing text sensors publish. Sources: last outbound byte already tracked
for the echo guard; last accepted decoded frame; `AuthoritySnapshot::state`;
`AuthoritySnapshot::speed_capability`; the provisioning record's sender id.

- [ ] **Step 4: Extend the codegen**

In `text_sensor.py`, extend `TEXT_SENSOR_SETTERS`:

```python
TEXT_SENSOR_SETTERS = {
    "command_status": "set_command_status_sensor",
    "evidence_source": "set_evidence_source_sensor",
    "last_tx_command": "set_last_tx_command_sensor",
    "last_rx_frame": "set_last_rx_frame_sensor",
    "last_confirmed_state": "set_last_confirmed_state_sensor",
    "speed_capability": "set_speed_capability_sensor",
    "remote_sender_id": "set_remote_sender_id_sensor",
}
```

In `sensor.py`, the existing schema hardcodes `UNIT_MINUTE` +
`DEVICE_CLASS_DURATION` for `timer_remaining`. Counters need neither. Replace
the single schema with a per-kind mapping:

```python
import esphome.codegen as cg
from esphome.components import sensor
import esphome.config_validation as cv
from esphome.const import (
    DEVICE_CLASS_DURATION,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_MINUTE,
)

from . import QuietCoolComponent

CONF_CONTROLLER_ID = "controller_id"
CONF_KIND = "kind"

_COUNTER_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=0,
    state_class=STATE_CLASS_TOTAL_INCREASING,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
)

SENSOR_KINDS = {
    "timer_remaining": (
        "set_timer_remaining_sensor",
        sensor.sensor_schema(
            unit_of_measurement=UNIT_MINUTE,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_DURATION,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    ),
    "tx_count": ("set_tx_count_sensor", _COUNTER_SCHEMA),
    "rx_valid_count": ("set_rx_valid_count_sensor", _COUNTER_SCHEMA),
    "rx_rejected_count": ("set_rx_rejected_count_sensor", _COUNTER_SCHEMA),
}


def _validate(config):
    kind = config[CONF_KIND]
    return SENSOR_KINDS[kind][1](config)


CONFIG_SCHEMA = cv.All(
    sensor.sensor_schema(entity_category=ENTITY_CATEGORY_DIAGNOSTIC).extend(
        {
            cv.Required(CONF_CONTROLLER_ID): cv.use_id(QuietCoolComponent),
            cv.Required(CONF_KIND): cv.one_of(*SENSOR_KINDS, lower=True),
        }
    ),
    _validate,
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    controller = await cg.get_variable(config[CONF_CONTROLLER_ID])
    cg.add(getattr(controller, SENSOR_KINDS[config[CONF_KIND]][0])(var))
```

If `_validate` proves awkward against the installed ESPHome's schema helpers,
fall back to a `cv.typed_schema`-style split keyed on `kind`; the requirement is
only that `timer_remaining` keeps its minutes/duration metadata and the counters
get none.

- [ ] **Step 5: Run tests and validate the config**

```bash
rm -rf tests/cpp/build && make -C tests/cpp test-adapter
cd /Users/bryanli/Projects/joyfulhouse/esphome-config \
  && .venv/bin/esphome config quietcool-lora32-downstairs.yaml | tail -3
```
Expected: adapter tests PASS; config still `INFO Configuration is valid!`
(the YAML does not use the new kinds yet — this only proves the codegen still loads).

- [ ] **Step 6: Commit**

```bash
git add components/quietcool/esphome/quietcool_component.h \
        components/quietcool/esphome/quietcool_component.cpp \
        components/quietcool/text_sensor.py components/quietcool/sensor.py \
        tests/cpp/adapter/diagnostic_publication_test.cpp
git commit -m "feat: publish the five text diagnostics and three counters"
```

---

### Task 5: The Fan Timer select entity

**Files:**
- Create: `components/quietcool/esphome/quietcool_timer_select.h` / `.cpp`
- Create: `components/quietcool/select.py`
- Modify: `components/quietcool/esphome/timer_command.h` / `.cpp` (option⇄selection, feedback mapping)
- Modify: `tests/cpp/Makefile:47` (add the new entity `.cpp` to the `filter-out`)
- Test: extend `tests/cpp/adapter/timer_command_test.cpp`

**Interfaces:**
- Produces:
  - `constexpr const char* kTimerOptions[6]` = `{"None", "1 hour", "2 hours", "4 hours", "8 hours", "12 hours"}`
  - `std::optional<TimerSelection> selection_for_option(const std::string&)`
  - `const char* option_for_selection(TimerSelection)`
  - `std::optional<const char*> timer_option_for_authority(const ::quietcool::AuthoritySnapshot&)`
- Consumes: `add_authority_publisher` (Task 2), `timer_command_from_intent` (Task 1).

`quietcool_timer_select.cpp` derives from `select::Select` and is therefore
excluded from the adapter test build, exactly like `quietcool_fan.cpp`. **All
decision logic lives in `timer_command.cpp`**, which *is* linked and tested; the
entity file only marshals between ESPHome types and those pure functions. This
is the issue #15 lesson: logic welded inside an unlinkable entity file can be
inverted with the suite still green.

- [ ] **Step 1: Write the failing tests**

Append to `tests/cpp/adapter/timer_command_test.cpp`:

```cpp
TEST(option_strings_round_trip_through_selection) {
  for (const auto* option : kTimerOptions) {
    const auto selection = selection_for_option(option);
    EXPECT_TRUE(selection.has_value());
    EXPECT_STREQ(option_for_selection(selection.value()), option);
  }
}

TEST(none_is_the_continuous_option) {
  EXPECT_EQ(selection_for_option("None").value(), TimerSelection::Continuous);
  EXPECT_STREQ(option_for_selection(TimerSelection::Continuous), "None");
}

TEST(an_unknown_option_is_refused_not_guessed) {
  EXPECT_FALSE(selection_for_option("3 hours").has_value());
  EXPECT_FALSE(selection_for_option("").has_value());
}

TEST(unknown_timer_authority_publishes_nothing) {
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::UnknownTimerAuthority{};
  EXPECT_FALSE(timer_option_for_authority(snapshot).has_value());
}

TEST(a_programmed_timer_publishes_its_duration) {
  ::quietcool::AuthoritySnapshot snapshot{};
  ::quietcool::ProgrammedDurationAuthority programmed{};
  programmed.duration = ::quietcool::Duration::Hours4;
  snapshot.timer = programmed;
  EXPECT_STREQ(timer_option_for_authority(snapshot).value(), "4 hours");
}

TEST(no_active_timer_publishes_none) {
  ::quietcool::AuthoritySnapshot snapshot{};
  snapshot.timer = ::quietcool::NoActiveTimerAuthority{};
  EXPECT_STREQ(timer_option_for_authority(snapshot).value(), "None");
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `rm -rf tests/cpp/build && make -C tests/cpp test-adapter`
Expected: compile failure — `kTimerOptions` not declared.

- [ ] **Step 3: Implement the option and feedback mappings in `timer_command.{h,cpp}`**

`timer_option_for_authority` visits the `TimerAuthority` variant:
`LocallyAnchoredTimerAuthority` and `ProgrammedDurationAuthority` → the option
for their `Duration` (a `Duration::Off` in either, which should not occur,
maps to `"None"` — never to a stop); `NoActiveTimerAuthority` → `"None"`;
`UnknownTimerAuthority` → `std::nullopt`.

- [ ] **Step 4: Write the entity**

`quietcool_timer_select.h` declares `QuietCoolTimerSelect : public Component,
public select::Select, public AuthorityPublisher`, with
`set_controller(QuietCoolComponent*)` calling `add_authority_publisher(this)`.

`control(const std::string& value)`:
1. `selection_for_option(value)`; if empty, log a warning and return without transmitting.
2. Read the fan's current confirmed on/level from the controller.
3. `controller_->request_state(timer_command_from_intent(selection, fan_on, level, command_speed_count))`.
4. Publish nothing — `publish_authority` is the only publication path.

`publish_authority` calls `timer_option_for_authority` and publishes the option
only when one is returned.

- [ ] **Step 5: Write the codegen**

Create `components/quietcool/select.py` following `fan.py`'s shape: a
`select.select_schema(QuietCoolTimerSelect)` extended with the required
`controller_id`, a `cg.add_global(cg.RawStatement('#include
"quietcool/esphome/quietcool_timer_select.h"'))`, `await
select.register_select(var, config, options=list(TIMER_OPTIONS))`, and
`cg.add(var.set_controller(controller))`, where `TIMER_OPTIONS` is the same six
strings in the same order as `kTimerOptions`.

- [ ] **Step 6: Exclude the entity from the adapter link**

In `tests/cpp/Makefile:47`, extend the filter:

```make
ADAPTER_SOURCES := $(filter-out %/quietcool_fan.cpp %/quietcool_button.cpp \
	%/quietcool_timer_select.cpp, \
	$(wildcard $(ROOT)/components/quietcool/esphome/*.cpp))
```

Update the comment above it to name the new file and why.

- [ ] **Step 7: Run tests to verify they pass**

Run: `rm -rf tests/cpp/build && make -C tests/cpp test && make -C tests/cpp test-adapter && make -C tests/cpp test-sanitized`
Expected: all PASS.

- [ ] **Step 8: Mutation-test the feedback and refusal paths**

1. Make `selection_for_option` return `TimerSelection::Continuous` for unknown input.
   Expected: `an_unknown_option_is_refused_not_guessed` FAILS.
2. Make `timer_option_for_authority` return `"None"` for `UnknownTimerAuthority`.
   Expected: `unknown_timer_authority_publishes_nothing` FAILS.

Revert both.

- [ ] **Step 9: Commit**

```bash
git add components/quietcool/esphome/quietcool_timer_select.h \
        components/quietcool/esphome/quietcool_timer_select.cpp \
        components/quietcool/esphome/timer_command.h \
        components/quietcool/esphome/timer_command.cpp \
        components/quietcool/select.py tests/cpp/Makefile \
        tests/cpp/adapter/timer_command_test.cpp
git commit -m "feat: Fan Timer select entity, confirmation-driven"
```

---

### Task 6: YAML bindings and the binding tests

**Files:**
- Modify: `quietcool-cpp-lora32.yaml` (select block; eight diagnostics; header note)
- Modify: `tests/test_quietcool_esphome_config.py` (new `CppConfigBindingTest` class)

There is currently **no** test class asserting anything about
`quietcool-cpp-lora32.yaml` — the suite targets `legacy/quietcool-lora32.yaml`.
That gap is exactly how issue #28 happened: `Controller Fault` was implemented,
tested, reviewed and attested, and never reached the hardware because no YAML
line declared it. This task closes it for every entity in the shipped C++ config.

- [ ] **Step 1: Write the failing tests**

Add to `tests/test_quietcool_esphome_config.py`:

```python
CPP_CONFIG = ROOT / "quietcool-cpp-lora32.yaml"

_CPP_EXPECTED_ENTITIES = {
    "Fan Timer",
    "Last TX Command",
    "Last Valid RX Frame",
    "Last Confirmed Fan State",
    "Fan Speed Capability",
    "Remote Sender ID",
    "TX Count",
    "RX Valid Count",
    "RX Rejected Count",
}

_CPP_DIAGNOSTIC_ENTITIES = _CPP_EXPECTED_ENTITIES - {"Fan Timer"}


class CppConfigBindingTest(unittest.TestCase):
    """Implementation without a YAML binding is indistinguishable from no
    implementation (issue #28). Every entity the component can drive must be
    declared in the shipped C++ config."""

    def setUp(self) -> None:
        self.text = CPP_CONFIG.read_text()

    def test_every_new_entity_is_declared(self) -> None:
        for name in sorted(_CPP_EXPECTED_ENTITIES):
            with self.subTest(entity=name):
                self.assertIn(f'name: "{name}"', self.text)

    def test_every_restored_diagnostic_is_disabled_by_default(self) -> None:
        for name in sorted(_CPP_DIAGNOSTIC_ENTITIES):
            with self.subTest(entity=name):
                block = _entity_block(self.text, name)
                self.assertIn("disabled_by_default: true", block)

    def test_the_timer_select_is_not_disabled_by_default(self) -> None:
        block = _entity_block(self.text, "Fan Timer")
        self.assertNotIn("disabled_by_default: true", block)

    def test_the_timer_select_offers_exactly_the_supported_durations(self) -> None:
        block = _entity_block(self.text, "Fan Timer")
        self.assertIn("platform: quietcool", block)
        for option in ("None", "1 hour", "2 hours", "4 hours", "8 hours", "12 hours"):
            with self.subTest(option=option):
                self.assertIn(option, block)

    def test_the_config_documents_off_versus_continuous(self) -> None:
        # The distinction is counter-intuitive enough that a maintainer editing
        # this file must meet it here, not only in the design doc.
        self.assertIn("Continuous", self.text)
        self.assertIn("Off", self.text)
```

Add a module-level helper `_entity_block(text, name)` that returns the YAML
list item containing `name: "<name>"` — from the preceding `- platform:` line
up to the next line at the same indentation — so per-entity assertions cannot
be satisfied by a keyword appearing elsewhere in the file.

The per-entity block helper is what keeps these honest: without it,
`assertIn("disabled_by_default: true", self.text)` would pass on any file that
disables *some* entity, which is the same false-green that let issue #28
through.

- [ ] **Step 2: Run to verify they fail**

Run: `python3 -m unittest tests.test_quietcool_esphome_config -v 2>&1 | tail -20`
Expected: FAIL — the entities are not declared yet.

- [ ] **Step 3: Declare the entities in the YAML**

Add a `select:` block for `Fan Timer` (with a description naming the energizing
behavior), and the eight diagnostics under the existing `text_sensor:` and
`sensor:` blocks, each with `disabled_by_default: true`. Extend the file's
header comment: delete the now-false line "No Fan Timer select entity yet",
delete the diagnostics-not-carried-over paragraph, and add a short note that
`Off` stops the fan while `Continuous` runs it, pointing at the design doc.

- [ ] **Step 4: Run tests and validate both device configs**

```bash
python3 -m unittest tests.test_quietcool_esphome_config -v 2>&1 | tail -5
cd /Users/bryanli/Projects/joyfulhouse/esphome-config
.venv/bin/esphome config quietcool-lora32-downstairs.yaml | tail -3
.venv/bin/esphome config quietcool-lora32-upstairs.yaml | tail -3
```
Expected: unittest OK; both configs `INFO Configuration is valid!`; the resolved
output still shows `type: esp-idf`, `version: 5.5.5`, `loop_task_stack_size: 16384`,
and the correct sender per unit (`0xCB004739` downstairs, `0xCB03D7D3` upstairs).

- [ ] **Step 5: Mutation-test the binding guard**

Delete the `name: "TX Count"` declaration from the YAML. Expected:
`test_every_new_entity_is_declared` FAILS naming `TX Count`. Restore it.
Then remove one `disabled_by_default: true`. Expected:
`test_every_restored_diagnostic_is_disabled_by_default` FAILS. Restore.

- [ ] **Step 6: Commit**

```bash
git add quietcool-cpp-lora32.yaml tests/test_quietcool_esphome_config.py
git commit -m "feat: declare the timer select and eight diagnostics, and test the bindings"
```

---

### Task 7: Documentation and compile verification

**Files:**
- Modify: `README.md` (entity table)
- Modify: `INSTALL.md` (timer usage and its energizing warning)
- Modify: `docs/claude/2026-07-28-timer-control-and-diagnostics-design.md` (status → implemented)

- [ ] **Step 1: Update the docs**

README's entity table gains `Fan Timer` and the eight diagnostics, marked
disabled-by-default. INSTALL.md gains a short "Timers" section stating
plainly: selecting any timer duration **runs the fan**, selecting it on a
stopped fan **starts** the fan at LOW, and `None` means "no timer — run until
stopped", which **restarts** a fan whose timer already expired. Reference the
open-window requirement already documented for fan operation.

- [ ] **Step 2: Full local verification**

```bash
cd /Users/bryanli/Projects/joyfulhouse/esphome-quietcool
rm -rf tests/cpp/build
make -C tests/cpp test && make -C tests/cpp test-adapter && make -C tests/cpp test-sanitized
python3 -m unittest tests.test_quietcool_esphome_config 2>&1 | tail -3
cd /Users/bryanli/Projects/joyfulhouse/esphome-config && .venv/bin/esphome compile quietcool-lora32-downstairs.yaml 2>&1 | tail -6
```
Expected: every suite PASS; `INFO Successfully compiled program.`; RAM and
flash figures reported (compare against the pre-change baseline of RAM 33.9%,
Flash 55.5% and note the delta).

- [ ] **Step 3: Commit and open a PR**

```bash
git add README.md INSTALL.md docs/claude/2026-07-28-timer-control-and-diagnostics-design.md
git commit -m "docs: timer control usage and its energizing behavior"
git push -u origin feat/timer-control-and-diagnostics
gh pr create --title "feat: timer control and restored diagnostics" --body-file <(...)
```

- [ ] **Step 4: Adversarial review gate**

Per the standing rule, before any flash: run the four-engine adversarial review
(Fable high, Opus xhigh, Codex gpt-5.6-sol xhigh, Gemini 3.1 Pro) over the
branch diff, loop until all attest clean, and only then propose flashing.

- [ ] **Step 5: Hardware verification — REQUIRES AN OPEN WINDOW**

Flashing and the boot query are non-energizing and may proceed once review is
clean. **Exercising the timer is not.** Ask the maintainer to confirm a window
is open, then verify on downstairs (`10.100.8.46`): select "1 hour", confirm
`Command Confirmation Status` reaches `confirmed`, `Timer Remaining` becomes
non-empty, and the select republishes `1 hour` from confirmed authority rather
than optimistically. Then return the fan to its prior state.

---

## Self-Review

**Spec coverage.** §1 duration semantics → Task 1 (`TimerSelection`, the
`Continuous` fallthrough) and Task 6 (config header note). §2 D1 → Task 1
`stopped_fan_defaults_to_low_and_starts`. D2 → Task 5 `none_is_the_continuous_option`.
D3 → Task 5 option strings, Task 6 YAML. D4 → Task 6
`test_every_restored_diagnostic_is_disabled_by_default`. §3.1 → Task 1. §3.2 →
Task 5. §3.3 → Task 2. §3.4 → Task 5 feedback tests. §3.5 → Tasks 3 and 4. §4 →
Task 6. §5 → mutation steps in Tasks 1, 2, 3, 5, 6. §6 → Task 7 steps 4-5.

**Type consistency.** `TimerSelection` (Task 1) is the only enum crossing task
boundaries and is used identically in Tasks 1 and 5. `add_authority_publisher`
(Task 2) is consumed by Task 5's `set_controller`. Setter names in Task 4's
Python maps match the C++ setters declared in Tasks 3 and 4.

**Known rough edge.** Task 4's `sensor.py` restructure depends on the installed
ESPHome's schema-composition helpers; the task names an explicit fallback
rather than assuming the first form validates.
