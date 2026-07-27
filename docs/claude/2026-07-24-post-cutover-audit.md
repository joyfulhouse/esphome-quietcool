# Post-cutover audit — QuietCool C++ component (2026-07-24)

Seven parallel review passes run against the live-deployed C++ ESPHome
component after the production cutover: six Claude agents (core correctness,
adapter correctness, adversarial hardening, DRY/simplification,
performance/memory, test-evidence quality) plus an independent Codex
(gpt-5.6-sol, xhigh) covering all dimensions for cross-check.

Reviewed artifact: the C++ core integration branch (since merged into
`main`) at HEAD `13a1b3e` **plus the
uncommitted crash fixes** (5 modified sources, 3 untracked files).
Host suite green at **147/147** at time of audit.

Live device state during the audit: `10.100.8.46`, API reachable, uptime
climbing monotonically past 3,784 s with **zero** panic/WDT/boot-reset lines —
no reboot since the flash.

---

## Verdict

**No Critical findings. One High** — raised by the independent Codex pass and
verified here; every Claude pass missed it because all six measured the *live*
configuration rather than what the repository hands you on the next build.

The crash mechanism that bricked the controller twice is closed in the
*deployed* firmware, and closed *structurally* rather than by tuning a margin.
The gap is that nothing carries that guarantee forward to the next deployment.

Three passes independently converged on that conclusion from different
evidence:

| Property | Evidence | Source |
|---|---|---|
| Reentrancy eliminated **within the core** | The former recursion (`apply_effect(RequestTxBurst)` → `Busy` → `on_tx_rejected` → recurse) is now deferred to the next `loop()` via `core_callbacks_`; `apply_effect` has no branch that calls `apply_effects` | adapter pass, verified independently on re-challenge; Codex concurs |
| Stack depth bounded **by construction** | Target-frame objdump: `reduce` is a graph root never re-entered from its own subtree; `TrackCandidate` is `break` with a ≤2-iteration loop; no recursion/`alloca`/VLA in the namespace. Deepest chain ≈5.2 KB against 16 KB | hardening pass |
| Drain queue cannot overflow | One reduction emits ≤4 effects (`CoreEffects::kCapacity=4`); queue is never re-enqueued mid-`drain()`. Depth **≤4 of 16** | adapter pass, derived independently |

The 16 KB loop stack is genuine headroom, not a larger cushion under an
unbounded path.

**Scope correction, from the Codex cross-check.** "Structurally eliminated" is
correct *inside the core* but overstated as a whole-system claim. `CoreEffectDrain`
prevents a nested drain, but public component entry points call into `core_`
**before** enqueueing their returned effects, and ESPHome's `publish_state()`
callbacks are synchronous. So a user entity automation can enter core APIs
while a prior effect batch is still draining
(`quietcool_component.cpp:61,91`; `esp_event_sink.cpp:121`;
`quietcool_fan.cpp:53`).

Concrete scenario: a `command_status` `on_value` automation that requests
Refresh whenever status becomes `"refused"`. Each refusal publishes
synchronously, the automation issues another Refresh, the nested drain returns
via its guard, and the outer drain consumes the newly enqueued refusal —
looping until a watchdog reset, with queue occupancy never exceeding ~1 (so the
overflow protection never trips).

The shipped YAML contains no such automation, so this is **configuration-
dependent, not presently reachable**. The accurate formulation is: reentrancy
is eliminated *within the core*, and not yet structurally enforced *at the
component boundary*. The invariant that is actually required — no new core
input until every effect and publication from the previous input completes — is
not enforced by construction.

---

## HIGH — the fix is deployed but not encoded; the repo will reproduce the crash

Two independent gaps compound into one real risk.

**(a) Both checked-in example configs still select the 8 KB stack.**
`quietcool-cpp-example.yaml:14-15` and `quietcool-cpp-example-sx126x.yaml:15-16`
both specify `framework: type: arduino` with **no `loop_task_stack_size`** — so
they get ESPHome's default **8,192 B**. Measured peak consumption is
**6,304 B**, leaving ~1.9 KB. That is *precisely* the configuration that
crash-looped into safe mode and then died on the first Refresh.

Nothing encodes the requirement: no component schema validation, no build-time
assertion, no automated target-frame budget. The 16 KB lives only in the
un-versioned live-copy YAML.

**Concrete scenario:** the second (upstairs) controller, or any rebuild from
the examples, is flashed from a checked-in config and reproduces the original
kernel-corruption crash — with the reducer already iterative, so the obvious
suspect looks innocent.

**(b) CI does not run the host suite, so the regression tests are not a gate.**
`.github/workflows/ci.yml` runs `python -m unittest
tests.test_quietcool_esphome_config` and `esphome config`/`compile` against
`quietcool-lora32.yaml` and `quietcool-lora-v3.yaml` (now under `legacy/`) —
**the pre-port YAML
targets**. It never invokes `make -C tests/cpp test` and never compiles either
C++ example.

Consequence: restoring the `dispatch(TrackCandidate) → reduce()` recursion
fails the local probe but **leaves repository CI green**. Reintroducing inline
`core_.on_tx_rejected()` / `core_.on_radio_recovered()` inside `apply_effect()`
passes all 147 host tests anyway, because `esphome/*.cpp` is excluded from the
test target.

**Fix (all safe-anytime, none touch the device):**

1. Set `framework: type: esp-idf` with
   `advanced: loop_task_stack_size: 16384` in **both** examples, with a comment
   explaining the measured requirement.
2. Add `make -C tests/cpp test` to CI, and add `esphome compile` of both C++
   examples.
3. Encode the minimum as a build-time check rather than a comment — a codegen
   validation that rejects an Arduino build, or a static assertion on the
   configured stack size.

This is the highest-value finding of the audit: the deployed firmware is safe,
but the *repository* currently cannot keep it that way.

## The finding that matters most: a latent crash class

**The consensus decision is encoded twice, and its destination state is
written twice.**

- Decision: `transaction_guard_matches` (`confirmation_core.h:234-246`)
  selects the action via guard predicates; `ObservationPolicy::decide_on/off`
  re-derives the same decision as variant outcomes
  (`confirmation_observation.cpp:85-111`).
- Destination state: the consensus rules hard-code
  `NextStateId::ResponseTailQuarantine`
  (`transition_table.cpp:88, 92, 95, 98, 101`, and `:118-119`), while
  `enter_tail` *also* sets `state_` **and** `context_` atomically
  (`confirmation_core.cpp:287-288`).

Mechanism, verified in source: `reduce()` runs the handler
(`confirmation_reducer.cpp:99`) *before* the table forces the state
(`:102-103`). The two encodings agree today — verified case-by-case. If they
ever drift, `apply_consensus` takes the `!policy_matches` early-return
(`confirmation_observation.cpp:112-116`) **without** calling `enter_tail`, so
`context_` keeps the previous alternative while the table still advances
`state_` to `ResponseTailQuarantine`. The next event does a throwing
`std::get<TailQuarantineContext>` → with `-fno-exceptions` on ESP-IDF that is
`std::terminate` → **reboot of a live fan controller**.

This is structurally the same defect that killed four consecutive YAML fix
rounds: one meaning encoded in two places, with a distant writer preserving
the old one. It is currently unreachable — but it fails as a hard crash
rather than a safe no-op, and nothing fails if the two copies drift.

### The branch is provably dead today — by exhaustion, not by inspection

Both encodings consume the **same six inputs**, computed identically:
`semantic_match`, `requested.is_on`, `consensus.is_on`, `command_marker`,
`prior_relation`, `attempts_remain`. Nothing mutates the transaction between
`select_rule` and `apply_consensus` in the reduce loop. An exhaustive
case-by-case comparison of guard-selected action against policy-derived
outcome:

- **ON path:** `SemanticMatch`→`ConfirmAndPromote` ✓; `yield` (CO&CM&PR≠Equal)
  →`YieldToOem` matches the policy's exact yield condition ✓; `stale`
  (CO&CM&PR=Equal&AR)→`RetryWithoutPromotion` ✓; `!yield,!stale,AR`
  →`ApplyMismatchWithRetry`, which `policy_matches` also accepts ✓;
  `!AR`→`ExhaustMismatch` ✓.
- **OFF path:** `yield`/`stale` are both RO-gated and therefore always false,
  so the guard reduces to `SM`→Confirm, else `AR`→Retry, else→Exhaust —
  matching `decide_off` exactly in all three cases ✓.

Coverage is total (for `SM=false`, one of yield/stale/RMAR/RME always fires),
so `select_rule` never returns null here either. **`policy_matches` is true
for every reachable input; the `!policy_matches` branch is dead code**, and
every live path through `apply_consensus` ends in `enter_tail`, so `context_`
always matches the forced `state_`.

### Why it is still worth fixing: the trigger is attacker-controlled

The *observed* half of that decision — `consensus.state` and its capability
bits — is **fully attacker-controlled**. During a post-command or fallback
listening window (open ~400–1600 ms after the bridge's own command TX), anyone
in RF range can inject 2–3 crafted response frames and drive consensus to any
chosen state. The RF link is unauthenticated by protocol, so this needs no
credential.

That converts the maintenance risk into a security one: **if any future edit
breaks guard ≡ policy for even one input combination, it becomes a remotely
triggerable crash loop on live hardware** — an attacker picks the diverging
combination and replays it. The defensive branch that exists to catch
divergence currently makes it *worse*, because it falls through to a forced
transition with a stale context instead of failing safe.

### Severity disagreement, recorded rather than smoothed

Codex independently found this same defect by source analysis, reached the same
conclusion ("agree today… drift hazard rather than a presently reachable RF
input") and the same failure mechanism — but rated it **Low**. This document
treats it as the most important finding.

The disagreement is about reachability of the *trigger*, not about the
mechanism. Codex weighed it as a maintenance hazard. The higher rating here
rests on the observation that `consensus.state` is attacker-controllable by RF
injection during the listening window, which converts a future drift from "a
bug someone eventually notices" into "a remotely selectable crash loop." Both
readings are defensible; the fixes below are cheap enough that the disagreement
does not need resolving before acting.

### Fixes, in recommended order

1. **Pin the equivalence with an exhaustive test — `safe-anytime`.** The input
   domain is only **96 combinations** (5 bools × `PriorRelation{3}`).
   Enumerate all of them and assert the guard-selected action equals the
   policy-derived action, reusing `matching_transaction_rules_for_test`
   (`confirmation_introspection.cpp:90`). Test-only; no device code; drift
   fails CI.
2. **Make the defensive branch fail safe — `needs-retest`.** Today
   `!policy_matches` returns without establishing a context. It should
   hard-fail into a resynchronized state — invalidate authority and
   `enter_tail(ReturnIdle)` so `context_` matches `state_` — rather than fall
   through to a forced transition with a stale context. This is the cheapest
   change that removes the crash *outcome*.
3. **Make `enter_tail` the sole writer — `needs-retest`.** Change those rules
   to `NextStateId::Computed` so the handler that already sets state+context
   atomically is the only writer. This removes the crash class *even if the
   encodings drift*, and deletes the duplicate state-destination encoding as a
   side effect. Verify first that every success branch of `apply_consensus`
   calls `enter_tail`.

(1) guards the decision; (2) and (3) guard the consequence. Given the trigger
is attacker-reachable, (1) and (2) together are the minimum defensible
position — (1) makes drift visible in CI, (2) makes drift non-fatal if it ever
ships anyway.

---

## Safety finding: failure is invisible in Home Assistant

`mark_failed()` (`quietcool_component.cpp:111,123,152`) is
`esphome::Component::mark_failed()` — it disables **only the controller's**
loop and is **reboot-only**; nothing invokes `reset_to_construction_state()`
at runtime.

The fan entity, sensors, display, WiFi and API are separate components and
keep running. So after a wedge:

- The fan entity **retains its last confirmed state (e.g. ON)** in HA,
  indefinitely.
- `state_known`, `command_status`, `evidence_source` all **freeze** — even the
  "authority lost" signal never flips.
- `Uptime`, `WiFi Signal` and the display keep updating, so the device looks
  entirely healthy.

Net effect for a whole-house fan: **it keeps running, HA shows it running, and
it can no longer be turned off — with no indication anything is wrong.**
Recovery requires OTA reflash or a physical power-cycle, because the new YAML
dropped the Restart button the pre-cutover build had.

Likelihood is very low — the only trigger is queue overflow, which is proven
unreachable (depth 4 vs capacity 16; RF cannot drive it; the sole user-config
route, a re-entrant `on_state` automation, does not exist in the shipped
YAML). But the consequence is loss of control of a live fan with zero warning.

Mitigations, ascending cost:

1. Restore `button: platform: restart` — one YAML line, restores in-HA recovery.
2. Expose controller failure as an HA `binary_sensor` — makes the wedge visible.
3. Publish fan / `state_known` as unavailable on failure — code change, correct fix.

---

## Evidence-base corrections

Three claims previously treated as established did not survive the audit.

**"ASan/UBSan clean" is not reproducible.** There is no `-fsanitize` anywhere
in `tests/cpp/Makefile`. A sanitized binary exists on disk, hand-built at
10:45, with no rule producing it and no gate running it. A fresh checkout plus
`make test` exercises neither sanitizer.

**The "10k-combo exhaustiveness" property is inflated ~100×.**
`transition_table_test.cpp:39` iterates 3 speeds × 7 durations × 256 raw bytes
× 3 prior × 2 attempts, but feeds only a *derived* boolean tuple — a domain of
**≤96 distinct points**. It asserts single-rule-match for only **2 of 31**
states. It proves table consistency, not semantic correctness.

**The §20 hardware-validation gate was skipped in full.** Forced-miss bench
exercise, forced watchdog trip, and the >7-trial report-timing measurement:
none performed. The deployment went Stage 2 → live single-coordinator control,
bypassing Stage 3 (RX shadow) and Stage 4 (TX-disabled shadow). The
`kPostCommandAcceptStartMs = 400` constant still rests on the original 7
trials, which the design document itself says are insufficient to lock for
release. This is a record of accepted risk, not a veto.

**Related margin check (resolved during the audit).** The adapter pass flagged
that `on_tx_complete`'s `now_ms` is sampled at loop-top, *before* frame 3's
blocking send, so the burst-completion anchor precedes true RF end by roughly
one frame's airtime (~40–55 ms). `ResponseWindow::post_command` anchors on that
`completed_ms` with an accept window of **[400, 1600] ms**. Reports measured at
+705..807 ms after true RF end therefore land at **755–857 ms** window-relative
— *further* from the 400 ms boundary, not closer. The skew is benign in both
directions (~355 ms margin at the open, ~740 ms at the close). Informational,
not a risk.

---

## Test coverage: the gap is where the risk was

*As found.* `quietcool_component.cpp` — which holds the **adapter half of the
crash fix** — had **zero host-test coverage**. The Makefile's `CORE_SOURCES`
excluded all of `esphome/` and both radio adapters, so those files were
verified only by `esphome compile` and live uptime.

The core half was already well tested: a `-finstrument-functions` recursion
probe fails if the reducer recursion is reintroduced, and `CoreEffectDrain`
reentrancy and queue-full behaviour have real unit tests.

Remaining highest-value tests, in priority order:

1. `reducer_iterative_equivalence_test` — replay the REG + fuzz corpus through
   both a reference recursive model and the iterative reducer; assert identical
   traces. The rewrite is currently backstopped only by unchanged tests plus
   the recursion probe, so behaviour-preservation is inferred, not proven.
2. `fuzz_recursion_probe` — extend the probe to the whole arbitrary-sequence
   loop rather than only REG-D, covering the Refresh→query→consensus path that
   produced the second crash.
3. `adapter_queue_overflow_marks_failed_test` — the overflow path is now
   reachable in the adapter suite but is not yet driven to overflow, because
   forcing >16 queued effects requires a core state that emits repeatedly
   without draining. Worth constructing deliberately.

Closed since: the adapter deferral tests (`bdc2bb1`) and the guard/policy
equivalence test (`e08a9f3`).

---

## Build reproducibility

**The production build cannot be reproduced with the installed toolchain.**

- Installed ESPHome (both the uv tool and the `.venv`): **2025.11.5**
- Flashed firmware self-reports: **ESPHome 2026.7.0**
- A fresh `uv tool run` today resolves to **2026.7.2** — a third version

Under 2025.11.5 the production config does not even validate:
`[platform] is an invalid option for [image]` at line 358 (2025.11.5's
`image/__init__.py` has no `CONF_PLATFORM` handling).

All four historical packaging fixes were re-verified against 2026.7.0 and
2026.7.2 and **all four still hold** — the namespace fix in particular remains
load-bearing rather than accidental (`esphome_ns = global_ns` is unchanged in
both, so the fully-qualified declaration is still required).

Recommendation: **pin the ESPHome version explicitly** so a recompile cannot
resolve to a toolchain that breaks the `image` platform.

### Correction: the rollback path was broken, and worse than reported

This document originally claimed the rollback config validated cleanly under the
installed 2025.11.5, "so emergency rollback is not broken." **That was wrong.**
The check behind it grepped command output for a specific error string rather
than testing the exit status, and silently passed.

Re-tested properly on 2026-07-24, every config — the live YAML build, the
upstairs wrapper, *and* the rollback backup — **fails under 2025.11.5**. The
`image: platform: file` syntax needs 2026.7.x. The version pin now added to all
three configs turns that into a clear message instead of a schema error.

Two further defects surfaced in the rollback rigging itself, both of which would
only have appeared mid-emergency:

1. `secrets.yaml` resolves relative to the **config file**, not the working
   directory, so the backup directory could not read it.
2. `external_components` pointed at a relative `components` directory that did
   not exist inside the backup.

Both are fixed with symlinks to the parent (no credentials duplicated), and the
rollback config now validates. `backup-2026-07-24-pre-cpp/README.txt` records
the working commands, the toolchain requirement, and the serial fallback that
needs no ESPHome at all.

The lesson matches the rest of this audit: **an untested recovery path is not a
recovery path.** The rigging had been in place all day and had never been
exercised beyond copying files into it.

---

## Maintainability

- **Magic `31` (the state count) hand-repeated** at `transition_table.cpp:123,
  132, 147, 163, 174` and `:271`. Adding a 32nd state silently skips the
  universal rules and `valid_rules()` still passes. Fix: a
  `constexpr kCoordinatorStateCount` beside the enum — value unchanged, table
  byte-identical.
- **Two hand-written bounded FIFOs.** `CoreEffectQueue`
  (`confirmation_core.h:48-76`, cap 16) and `CoreCallbackQueue`
  (`core_callback_queue.h:21-52`, cap 8) are the same ring-buffer concept with
  the same off-by-one-prone index math, both written under crash pressure.
  Fix: one `template<typename T, std::size_t N> RingBuffer`.
- **Dead code in the crash-fix hot path.** `deferred_fixed_state`
  (`confirmation_reducer.cpp:61, 93, 78-79, 104-105`) is provably always
  `nullopt` — both `TrackCandidate` rules use `NextStateId::Computed`. It
  obscures the continuation contract.
- **Comment rot on a capacity invariant.** `confirmation_core.h:26-28` claims
  "at most three effects… keep one spare slot"; the real maximum is exactly 4
  with **zero** headroom, and `add()` drops silently on the 5th.
- **Test boilerplate:** `sender()` redefined in 18 files, effect-extraction
  loops in 13, `holds_alternative` counters in 7. A shared fixture header
  collapses ~50 duplicated definitions. `safe-anytime`.

**Checked and clean:** timing constants are single-sourced in `core_types.h`;
`kRuleCount` self-checks at compile time; `state_name()` and
`fixed_next_state()` have no `default`, so `-Werror -Wswitch` catches a new
enumerator; `kQueryStates` already unifies the four query families as data; the
typed-state design is intact — nobody collapsed a typed decision back into a
bool. No over-abstraction found: every port has both a real adapter and a test
double.

---

## Security posture

Assessed against the correct bar — **not worse than the OEM remote**. The
433.92 MHz link is unauthenticated by protocol, CRC and whitening are off, and
any frame carrying the sender ID is trusted.

Every attacker capability in RF range (command the fan directly, force
authority yield, jam the response window, hijack the sender during a learn
window) is **inherent to the OEM protocol** and equally available against the
real remote. Nothing is made worse here, and one thing is structurally better:
**the bridge can never be used as a relay or amplifier** — it only ever
transmits `encode_query`/`encode_state` output and never retransmits received
bytes.

The one place that bar could be lost in future is the guard/policy equivalence
described above: because an attacker can drive `consensus.state` by frame
injection, a drift between the two encodings would hand them a remote crash
loop — a capability the OEM remote does not confer. That is the security
argument for fixing it now while it is still theoretical.

Malformed RF was swept clean: 0/1/5/6/7/255-byte frames, all-zero and all-0xFF
payloads, and undefined command bit patterns all size-reject or fall through to
`InvalidOrIrrelevant` with empty effects. No path to unintended TX or
unintended HA publish. Corrupt/missing NVS fails safe to unprovisioned (a wrong
sender ID cannot be restored — `SenderId::from_bytes` rejects any prefix ≠
`0xCB`), so the device can never command a neighbour's fan.

---

## Performance and memory

All figures are target-frame, from the confirmed-live ELF (ESP-IDF, `-Os`,
16 KB loop stack; `text 839,811 / data 188,844 / bss 43,985`).

**Worst-case stack ≈6.3 KB of 16,384 — ~62% margin.** Two passes derived this
independently and it reproduces the measured 6,304 B diagnostic peak exactly,
which validates the method. The deepest chain is
`loop`(768) → `reduce`(976) → `dispatch`(1040) → `handle_command_request`(2128)
→ `begin_transaction`(1088), plus ~0.9 KB of ESPHome loop-dispatch frames.

On the old 8 KB Arduino stack that same chain left **1,888 B (23%)** — which is
the quantitative explanation of the crash history and of why 16 KB was
required. Zero recursion cycles anywhere in the component; `reduce` iterates at
most twice. The margin is over a **provably bounded** path.

Top frames: `handle_command_request` 2128 · `issue_query` 1184 ·
`issue_command` 1152 · `begin_transaction` 1088 · `dispatch` 1040 ·
`reduce` 976 · `loop` 768.

**Top stack lever (Medium, not currently needed):** `CoreEffects` is **648 B on
target** and is returned **by value**, accounting for ~83% of the peak via ~6
co-resident copies down the deepest chain. `finish_transaction` already uses
the good pattern (`CoreEffects&` out-param); converting `begin_transaction` and
`defer_command` to match would cut ~0.65–1.3 KB from the deepest frame.
Unnecessary at 62% margin — hold it in reserve.

**RAM:** ~4.3 KB static BSS (controller instance 4,016 B + fan/buttons). The
transition table (4,284 B) is correctly in `.flash.rodata`, **not** RAM.
No memory pressure.

**Heap:** effectively zero-alloc as designed. Sole deviation is a 6-byte
`std::vector` per *transmitted* frame (3 per burst), forced by ESPHome's
`transmit_packet(std::vector)` signature. Off the RX/consensus path entirely.

**Loop blocking — one item worth knowing, and it is upstream, not ours.**
`BurstTransmitter` is non-blocking (one frame per `loop()`, gaps are deadline
checks). But `SX127x::transmit_packet` busy-waits on
`while(!dio0_pin_->digital_read())` with **no `feed_wdt`, no yield**. Normal
case <100 ms per frame — comfortable. Fault case (DIO0 stuck) spins for
**4,000 ms** against a configured `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`, leaving
only ~1,000 ms of margin on an unfed spin. A fault spin stacked with other
components' loop work in the same iteration could cross 5 s. This is ESPHome's
driver, not this component — worth knowing, not a component fix.

**`sram1_as_iram: true` (+40 KB) — recommendation: DEFER.** The boot log
advertises it and bootloader support is confirmed, but it reclaims **IRAM, not
DRAM**, and this build is not IRAM-constrained (it links fine without it). The
16 KB loop stack is DRAM-heap allocated, so it does not relieve the constraint
that actually mattered here. Taking it would require reflashing stable live
hardware for headroom with no current consumer. Record it as available; bundle
it into the next necessary reflash if a future feature hits a limit.

---

## Remediation status (2026-07-24, after the audit)

All repository-side findings are fixed and committed on the C++ core
integration branch (since merged into `main`).
**Nothing was flashed** — the live controller still runs the 11:26 build, and
every change below takes effect at the next deliberate flash.

| Finding | Status | Commit |
|---|---|---|
| Crash fixes uncommitted and unbacked-up | Fixed | `d3b6e7c` |
| **HIGH** — examples select the 8 KB Arduino stack | Fixed | `19251cd` |
| **HIGH** — CI never ran the host suite or C++ examples | Fixed | `19251cd` |
| Stack minimum not enforced anywhere | Fixed — codegen rejects the build | `19251cd` |
| "ASan/UBSan clean" not reproducible | Fixed — `test-sanitized` target, gated in CI | `19251cd` |
| Consensus divergence branch not fail-safe | Fixed | `e08a9f3` |
| Guard/policy equivalence unenforced | Fixed — exhaustive test | `e08a9f3` |
| **Test harness rebuilt nothing on header edits** | Fixed | `e08a9f3` |
| Hand-repeated state count `31` | Fixed — derived from the enum | `1352686` |
| Dead `deferred_fixed_state` in the reducer | Fixed — replaced by a `static_assert` | `1352686` |
| `context_for_test` `default:` hid new states | Fixed — `-Wswitch` now catches them | `1352686` |
| `CoreEffects` capacity comment rot | Fixed | `1352686` |
| Toolchain version unpinned | Fixed — `min_version` in all three configs | `f697c00` |
| Two duplicate bounded FIFOs | Fixed — shared `RingBuffer<T,N>` | `8c840ae` |
| **Adapter half of the crash fix untested** | Fixed — 8 host tests, mutation-verified | `bdc2bb1` |
| Restart button dropped from production YAML | Fixed — pending next flash | live copy |
| "IP Address" entity silently dropped | Fixed — pending next flash | live copy |

### Found during remediation, not during the audit

Mutation-testing the new equivalence test exposed a defect that made a large
part of this project's verification history unreliable: **`tests/cpp/Makefile`
listed only `*.cpp` as prerequisites**, so editing any header — where the guard
predicates, `CoreEffects` and the effect drain all live — left the previous
binary in place and the suite "passed" against stale object code.

The first mutation (deliberately breaking a consensus guard) changed nothing.
After headers became prerequisites, that same mutation failed both the new
equivalence test *and* the existing transition-table test — which had therefore
been silently passing against stale objects too. Headers are now prerequisites
of both the normal and sanitized targets.

This is worth remembering: a green suite proved less than it appeared to,
twice, for different reasons. That is why the target-build coverage of the new
`RingBuffer` was confirmed with a deliberate `static_assert(false)` probe
rather than assumed from a successful compile.

### The adapter gap, now closed

The largest coverage gap — `quietcool_component.cpp`, holding the adapter half
of the crash fix — is covered as of `bdc2bb1`. The scaffold turned out far
smaller than feared, because the adapter really is thin: all four adapter
translation units compile against seven mechanical stub headers under
`-Wall -Wextra -Werror`, and the classes that carry the logic
(`ConfirmationCore`, `BurstTransmitter`, `CoreEffectDrain`,
`CoreCallbackQueue`) are the real ones, already host-compiled.

The suite was mutation-verified rather than trusted: restoring the pre-fix
inline `core_.on_radio_recovered()` call inside `apply_effect()` — the exact
shape that corrupted the FreeRTOS ready lists — fails three of the eight tests.

`quietcool_fan.cpp` and `quietcool_button.cpp` remain excluded. Stubbing
`fan::Fan` and `button::Button` would be a reimplementation rather than a stub,
and a stub large enough to be wrong is worse than no test. Both stay covered by
`esphome compile`, and the publication gate they depend on is separately
unit-tested.

### Deliberately not done

- **Publishing entities as unavailable on `mark_failed`.** The Restart button
  restores recovery, but the wedge is still invisible in Home Assistant. Doing
  this properly means a new diagnostic entity kind — codegen, a new `kind:`
  value, and a YAML change that alters the entity set on a live system. That is
  a product decision rather than a defect fix, and it cannot be validated
  without a flash, so it is left for the next deliberate deployment.
- **`CoreEffects&` out-params** on `begin_transaction`/`defer_command`
  (~0.65–1.3 KB off the deepest frame) — unnecessary at 62% margin.
- **`sram1_as_iram`** — IRAM, not DRAM; the build is not IRAM-constrained.
- **Test-fixture deduplication** (~50 repeated definitions) — mechanical churn
  across 18 files with modest benefit; better as its own change.
---

## Recommended actions, in order

Nothing here is an emergency. The live build is sound; these are cheap
insurance against the failure modes the audit could identify.

**Do first — no device risk, no reflash:**

0. **Close the High.** Switch both example YAMLs to ESP-IDF + 16 KB, add
   `make -C tests/cpp test` and a C++-example compile to CI, and encode the
   stack minimum as a build-time check. Until this lands, the repository can
   hand you back the crash — most immediately if the upstairs controller is
   ever flashed from a checked-in example.
1. **Commit the crash fixes.** They are still uncommitted and unbacked-up, and
   they are the fix for a bug that bricked the controller twice.
2. **Add the 96-input guard/policy equivalence test.** Test-only. Converts the
   top latent finding from "correct by coincidence" into "drift fails CI."
3. **Add `-fsanitize=address,undefined` to a Makefile target and gate it.** The
   "ASan/UBSan clean" claim currently rests on a hand-built binary no rule
   produces.
4. **Add the two adapter host tests** (`apply_effects` reentrancy deferral;
   queue-overflow → `mark_failed`). This is the file with zero coverage that
   holds the adapter half of the crash fix.
5. **Pin the ESPHome version** so the build stops depending on an ephemeral
   uninstalled toolchain.
6. **Fix the comment rot** on the `CoreEffects` capacity invariant and delete
   the dead `deferred_fixed_state` branches.

**Bundle into the next necessary reflash — do not flash solely for these:**

7. **Restore `button: platform: restart`** to the production YAML (one line).
8. **Restore the dropped "IP Address" entity**, or document its removal.
9. **Make the `!policy_matches` branch fail safe** (invalidate authority +
   `enter_tail(ReturnIdle)`).

**Hold in reserve — no current need:**

10. `CoreEffects&` out-params on `begin_transaction`/`defer_command`
    (~0.65–1.3 KB off the deepest frame; unnecessary at 62% margin).
11. `sram1_as_iram: true` (+40 KB IRAM; the build is not IRAM-constrained).
12. The `RingBuffer` template unifying the two hand-written FIFOs, and the
    `kCoordinatorStateCount` constant replacing the hand-repeated `31`.

**Still owed to the system, not the code:** the §20 bench gates
(forced-miss, forced watchdog trip, >7-trial timing) and a first Refresh from
the mounted position. No amount of static review substitutes for these.

---

## Receiver inventory (2026-07-24)

Established while checking whether both units run the latest build. They are
**not on the same track**, which matters before any "update everything" action.

| | Downstairs | Upstairs |
|---|---|---|
| Address | `10.100.8.46` | `10.100.2.62` (mDNS resolves) |
| Config | `quietcool-cpp-lora32.yaml` | `quietcool-lora32-upstairs.yaml` |
| Track | **C++ component** | **YAML** — wraps `legacy/quietcool-lora32.yaml` as a package |
| Sender ID | `0xCB004739` (provisioned) | `0x00000000` — learns from its own remote |
| Running | ESPHome 2026.7.0, built 11:26:36 | not yet banner-checked |
| Rollback rigging | yes, now verified | **none** |
| API | open (healthy) | open (healthy) |

Consequences worth stating plainly:

- **"Latest" is ambiguous for upstairs.** It is a YAML-track device. Bringing it
  to the latest *C++* build is a migration, not an update: it needs its own
  wrapper over `quietcool-cpp-lora32.yaml`, and its unprovisioned seed means it
  must re-learn from the upstairs remote after flashing. Its OTA password was
  also rotated on 2026-07-20 with the documented compile-then-upload caveat.
- **Upstairs has no rollback binary.** Downstairs got one before its cutover;
  upstairs never did. Take one before changing it.
- **Downstairs is behind the repository**, but only by changes made *after* its
  11:26 flash — the consensus fail-safe, the reducer cleanup, the state-count
  derivation and the `RingBuffer` refactor. Those are real firmware changes and
  have not been through the adversarial-review gate that this project requires
  before a production RF flash; the audit reviewed the pre-change code.

---

## Methodology note: three confident claims were wrong

Worth recording, because the pattern recurred and the correction mechanism is
the reusable part.

1. **A prior review refuted the correct crash suspect** using host-frame stack
   measurements ~7× smaller than Xtensa target frames.
2. **The hardening pass refuted the `std::get` fragility** by citing
   `transition_table.cpp:78` as proof the lease handlers use
   `NextStateId::Computed`. Line 78 is the *`TrackCandidate`* rule; the lease
   rules use fixed next-states. Retracted, then replaced with a stronger
   exhaustive proof.
3. **The performance pass reported a High** claiming `reduce → dispatch` was
   absent from the flashed image — which would have meant the firmware silently
   ignores every command. Refuted by direct disassembly:
   `400ecb11: call8 400ec34c <dispatch>`.

Cause of (3) is worth internalizing for any future binary work here:
`call8` is **PC-relative**, so its encoding contains no absolute address to
grep for; and objdump's **linear sweep loses alignment** on variable-length
Xtensa, decoding the region as `lsi f0` and an orphan `.byte 0xff` so no
symbolized call line ever existed to match. Only `--start-address` at the exact
branch target recovers it. The pass's own data contained the resolution: 15
literals in the object vs 14 in the image, with the function exactly 8 bytes
smaller, is the signature of the linker **relaxing** `l32r`+`callx8` into a
direct `call8` — not of a dropped call.

**The mechanism that caught all three was re-deriving load-bearing claims
directly rather than counting agreement between reviewers.** Two of the three
wrong claims were *refutations* of correct findings, which is the more
dangerous direction: a false refutation quietly removes a real finding from the
report.

---

## Open item

The **`0xAF` MEDIUM mystery** from cutover attempt 1 remains unexplained.
Three independent passes confirmed no code path emits it at boot: `0xAF` is
`FanState::command(Medium, Continuous).outbound_command_byte()`, which requires
a user transaction that boot never creates; boot can only issue the `0x66`
query. Undetermined from code.
