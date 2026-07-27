# Adversarial fix round — QuietCool C++ component (2026-07-25)

A multi-agent fix round on the live-deployed C++ ESPHome component, following the
2026-07-24 post-cutover audit. Findings were raised by an adversarial review of
`main` (tracked under #14), split into one issue per defect (#5–#25), and each
fixed on its own worktree by a separate implementing agent, cross-reviewed by a
different model family (Claude and Codex `gpt-5.6-sol`), then merged onto an
integration branch.

Integration head `a8862b6`: host suite **182/182 core, 23/23 adapter**,
`make test-sanitized` (`-fsanitize=address,undefined`) 182/182.

This is a record, not a victory lap. Its most useful content is section 2 — the
confident claims that turned out wrong — because that is what a future reader
cannot reconstruct from the diff. Nothing here has been flashed to a device.

---

## 1. What was found and fixed

Every item below is merged into `a8862b6` unless marked open. `file:line`
anchors are as read during the round.

**Safety / correctness (the ones that could actuate or lie):**

- **#5 (H1) — classifier tolerance depended on which branch ran.** Only the
  in-window `Accepting` path reached the tolerant `recover_response`; every other
  path used `decode_strict`, so an overlength-but-intact frame or a single-bit
  header hit was dropped silently — an OEM-remote press left Home Assistant on
  the stale state forever. Fixed by resolving the frame once at the top of
  `classify()` (`resolve_frame`) and having every branch consume that result
  (`79e711d`).
- **#6 (H2) — Learn could bind and persist the wrong physical fan.** Fixed to
  fail closed when two fans are heard in the window and to require three
  sightings before binding (`e73eea6`).
- **#8 (M1) — the effect drain had no work budget.** A synchronous automation
  re-entering the drained queue could keep `loop()` from returning until the
  watchdog reset the board. Fixed with a per-`drain()` budget of 32 applies that
  defers the remainder to the next `loop()` (backpressure, nothing dropped)
  (`f3c4763`).
- **#9 (M2) — queue overflow failed the controller invisibly.** `mark_failed()`
  is permanent and silent; the fan card kept showing stale confirmed state.
  Fixed with `degrade()`: latch first, raise a "Controller Fault" problem
  sensor, invalidate the entity-layer authority flags, then `mark_failed()`
  (`f3c4763`).
- **#11 (L2) — cast-invalid typed states.** A `static_cast`-built `FanState`
  could transmit an undefined nibble or wedge the coordinator. Fixed by
  validating at core ingest, re-checking the duration nibble in `encode_state`,
  and making the lease rule own its next-state so an encode failure returns to
  `Idle` instead of wedging (`5846df9`).
- **#15 — the HA-intent → RF command mapping was linked into no test binary.**
  Inverting its ON/OFF → Duration mapping passed the entire suite. This was the
  most serious finding of the round: a wrong display cannot start a fan, but a
  wrong *command* can. Fixed by extracting the pure `fan_command_from_intent`
  and unit-testing it (`6611a1f`).
- **#18 — the reverse mapping and button dispatch were also unlinked.** Extracted
  `authority_to_feedback` (confirmed state → HA display) and
  `dispatch_button_press` (button kind → provisioning action) as pure functions
  and tested them. Buttons cannot command the fan; a swapped label is a misrouted
  provisioning action, not a hazard (`09f15ee`).
- **#19 — `clamp_fan_speed` was unsafe when `supported_speed_count` is 0.** Made
  total: the ceiling is floored at 1, so a degenerate count can never form an
  undefined speed nibble regardless of caller (`8054447`).
- **#22 — effects applied after `degrade()` overwrote the terminal publication.**
  A #9 regression: the drain kept applying the rest of the batch after degrading,
  so a trailing `RequestAccepted` republished `command_status` "pending" over
  "unavailable". Fixed with a terminal `halt()` on the drain, kept explicitly
  distinct from the #8 backpressure path (`e8cf852`).
- **#23 — `setup()` bypassed the degradation latch.** A #9 regression: every
  other public entry point was guarded; `setup()` was not, so an automation could
  re-enter `core_.restore()` on a broken core. Guarded (`e8cf852`).
- **#25 — `context_matches_state` ignored `TxReason`.** The invariant compared
  state/context without the reason family, and the test fixture built
  Manual/Fallback/Recovery contexts with `BootQuery`. Fixed to compare the
  `TxReason` *family* (see section 2, item 7) and the fixture corrected
  (`8dbd6c8`).

**Observability / diagnostics:**

- **#10 (L1) — a `0xCE` special diagnostic was classified then dropped.** Wired
  to a log-only `SpecialDiagnosticHeard` event: `PublishDiagnostic` /
  `NextStateId::Same`, no authority, timer, TX, or actuation (`79e711d`).
- **#20 — `recover_response` hardcoded the `0xCB` header** while `decode_strict`
  used the provisioned `byte[0]`. Fixed to derive the normal header from the
  provisioned sender, so the two decoders cannot diverge (`ae7773e`). Severity
  was overstated when filed — see section 2, item 6.

**Test / build integrity:**

- **#12 (L3) — three tests proved surrogate properties, not their named
  invariants.** Hardened so each asserts the invariant it claims (`c04a404`).
- **#21 — `-Werror` was environment-overridable**, weakening the gate that
  justified deleting a test in #12. Made the warning/error flags non-overridable
  in the test Makefile (`0faf069`).

**Documentation (correct behaviour that only needed recording):**

- **#7 (H3) — the OEM RF protocol is unauthenticated.** Reported fan state is
  corroborated by sender/window/epoch binding, not cryptographically
  authenticated. Recorded as a threat model rather than "fixed", because the
  binding it might have added was already present — see section 2, item 2
  (`561b812`).
- **#13 — the SX127x busy-wait WDT invariant** (one ≤4 s synchronous transmit per
  `loop()` iteration against the 5 s task WDT) documented (`171d8d8`).
- **#24 — C++-core pairing procedure** (three sightings, no OLED prompt)
  documented alongside the YAML build's procedure (`b305b8c`). The issue as filed
  was wrong — see section 2, item 4.

**Open by deliberate choice — recorded boundaries, not bundled into safety
fixes:**

- **#16 — require an explicit override to re-learn when a sender is already
  bound.** A hardening of the pairing UX. Left open so the safety fixes stayed
  narrow and reviewable; folding a workflow change into #6 would have widened its
  blast radius.
- **#17 — Learn can still bind a lone foreign fan if the intended fan is silent
  for the whole window.** A residual of #6: the three-sightings-and-fail-closed
  rule defends against *two* fans heard, not against *only the wrong one* being
  heard. Recorded as a known boundary of the current Learn design rather than
  papered over.

---

## 2. Seven confident claims that turned out wrong

This is the heart of the record. Each was stated with confidence, was
load-bearing, and was false. **None was caught by a passing test.** All were
caught by re-deriving the claim from source; twice by an implementing agent that
stopped and refused to execute a spec because it contradicted the code.

1. **"#5 is a promote-vs-invalidate bug" (Codex).** The framing was that a
   corrupted OEM report would be *promoted to authority* and needed to be made
   conservative. In fact `ExternalPriorityState` already routes to
   `assert_oem_priority(external_state=true)`
   (`confirmation_observation.cpp:5-27`), which *invalidates* authority, arms
   recovery, and enters `OemHoldoff` — HA shows unknown and re-queries. The
   conservative behaviour the finding asked to be built already existed; #5 was
   only ever about frames being *dropped before* reaching it. Two reviewers made
   this same inversion before it was pinned to the code.

2. **"Response-to-query binding is missing" (Codex).** Raised as a spoofing gap.
   Sender, response-window, and epoch binding were all present in the core. Once
   that was verified, the finding had no code remedy left — which is *why* #7
   became a documented threat model rather than a fix. The real, irreducible
   property is that the protocol is unauthenticated; the binding that exists
   corroborates, and that is as far as the physics allow.

3. **The round's own #5 repro used the wrong probe byte (the orchestrator).** The
   initial failing H1 reproduction used `0xCA` as its single-bit-corruption
   probe. `0xCA` is Hamming-1 from *both* `0xCB` (normal) and `0xCE` (special),
   so `recover_response` refuses it *by design* (the ambiguity tie-break). That
   test would have kept failing after a *correct* fix and read as an incomplete
   one — a false negative built into the acceptance criterion. The correct
   single-bit probe is `0x4B` (the bit-7 flip of `0xCB`); `0xCA` belongs in the
   suite only as a *rejection* guard. The shipped `idle_tolerance_test` uses both
   correctly.

4. **"#6 broke the documented pairing flow" (Codex #24, amplified by the
   orchestrator).** Codex claimed the #6 Learn change contradicted the pairing
   procedure in the README. The README documents a *different build* —
   `legacy/quietcool-lora32.yaml`, the legacy YAML state machine, which never
   compiles
   `components/quietcool/` at all. The C++ core and the documented procedure were
   never the same artifact. The orchestrator amplified the claim rather than
   catching it; the implementing agent stopped, read the actual build the README
   drives, and reduced #24 to "document the C++ core's *separate* procedure." A
   spec that contradicts the code was refused, not executed.

5. **"#12's array-OOB is demonstrated" (the orchestrator).** Stated as a
   reproduced out-of-bounds. It was *argued*, not demonstrated: ASan would not
   reproduce it at `-O2`, and no failing case was ever produced. Downgraded to
   what it was — a surrogate-property test weakness — which is the form #12
   actually took.

6. **"#20 is a live divergence" (the orchestrator).** The claim that
   `recover_response`'s hardcoded `0xCB` could diverge from the provisioned
   `byte[0]` was filed at a severity implying a reachable defect. `SenderId`
   only admits `0xCB`-prefixed IDs at the *type level*
   (`SenderId::from_bytes` rejects the rest), so the divergence was
   *unreachable*, not merely guarded at runtime. It was still fixed
   defensively (`ae7773e`), but as a maintainability alignment, not a live bug.
   Overstating reachability is a quieter error than a false refutation, but it
   still mis-prices the work.

7. **The #25 issue text specified a fix that was wrong (the orchestrator).** The
   issue asked for an *exact* `TxReason` comparison in `context_matches_state`.
   That would have *falsely rejected* two legitimate pairings —
   `RecoveryQueryInitial` and `RecoveryQueryRetry` both legitimately share a
   Recovery context. The implementer recognised this, used a `TxReason` *family*
   comparison instead, and was right. The issue author's proposed remedy would
   have introduced a new false-negative while closing a real one; the agent
   executing it declined the literal instruction in favour of the correct one.

**The pattern.** Four of the seven were *refutations or framings* that removed or
misdirected a real finding (items 1, 2, 4, 5) — the more dangerous direction,
because a false refutation quietly deletes a true finding from the report. The
correction mechanism in every case was the same: **re-derive the load-bearing
claim directly from the source, rather than counting agreement between
reviewers.** And in the two cases where the *specification itself* was wrong
(items 4 and 7), what caught it was an implementing agent treating "the spec
contradicts the code" as a stop condition, not a detail to reconcile silently.

---

## 3. Test integrity

Sixteen mutations were applied by hand across the round — one per load-bearing
test — each verified to fail the specific test that claims the property, then
reverted to green. The discipline is not decorative: this project has twice
shipped a green suite that proved less than it appeared to, and this round the
extraction issues (#15, #18) each began from a mutation — inverting a mapping —
that passed the *entire* suite because the mapped code was linked into no test
binary.

What the mutations pinned, by area:

| Area | Mutation | Pins |
|---|---|---|
| #5 tolerance | strip the `recover_response` fallback | branch-independent tolerance (the dropped-frame bug) |
| #5 ambiguity | weaken the `>=` tie-break | `0xCA`/`0xCF` stay rejected |
| #5 special query | drop the `0xCE` exclusion | special queries stay unheard |
| #10 diagnostic | delete the `SpecialDiagnostic` → event mapping | `0xCE` is logged, not dropped |
| #8 budget | remove the budget check; clear the queue on exhaustion | `loop()` is bounded; the remainder is preserved |
| #9 degrade | revert `degrade()` to a bare `mark_failed()` | overflow is visible, not silent |
| #15 command | invert ON/OFF → Duration | a "turn on" commands a running fan |
| #18 feedback | invert `is_on()` | a running fan reports on |
| #18 button | swap a case label | Refresh routes to refresh, not forget |
| #22 halt | remove the mid-drain halt check | no effect overruns the terminal publication |
| #23 setup | remove the `setup()` guard | a degraded core is not re-entered |

Two meta-lessons, both learned the hard way this round:

- **A mutation that fails to *compile* prints nothing, and a suite that never
  ran reads exactly like a suite that passed.** If a mutation trips `-Werror`
  (an unused variable, an unreachable statement) the build aborts before any
  test runs; a harness check that greps only for `FAIL` lines then sees none and
  reads green. The correct mutation must be a *behavioural* change that still
  compiles, and the runner's exit status — not the absence of `FAIL` — is the
  signal. This is also why #21 mattered: an overridable `-Werror` lets the same
  blindness in through the front door.

- **A redundant guard can shadow another guard, making its mutation
  unobservable.** In #22 the drain initially had both an entry check
  (`if (draining_ || halted_) return;`) and a `!halted_` term in the loop
  condition. Removing either one alone left the other to catch the case, so the
  "halt is terminal across drains" mutation *passed* — the test could not see
  its own guard removed. The fix was to make each guard uniquely load-bearing
  (delete the redundant `!halted_`) so the mutation became observable. A guard
  that shadows another guard is the structural twin of the compile-failure trap:
  in both, a green result certifies nothing.

---

## 4. The configuration migration

Two build-reproducibility defects, both closed:

- **Deployed devices built from an absolute path into a stale worktree.** The
  live configuration compiled `components/quietcool/` via an absolute filesystem
  path pointing 18 commits behind the integration branch. A clone of the repo did
  not reproduce what was running. Closed by making the C++ core the repo's
  primary, in-tree documented config (`19a92f0`) and renaming the legacy YAML
  component to `quietcool_legacy_yaml` (`307216f`).
- **`quietcool-cpp-diag.yaml` reached outside the repo**, so a clean checkout
  could not build it. Closed by the same migration.

All five configurations validate under **ESPHome 2026.7.2**. State this
precisely: that is **schema and codegen validation** (`esphome config`), which
confirms the YAML parses, the pins resolve, and the codegen wiring is
well-formed. **It is not C++ compilation** — see section 5.

---

## 5. What is still not proven

The value of this document is that it is honest about its limits. As of
`a8862b6`:

- **No on-device testing.** Nothing in this round has been flashed. Every claim
  is from the host suite, static reading, and schema validation. The host suite
  models the ESPHome entity surface with stubs; it is not the firmware.
- **The C++ core is not compiled by CI here.** The migration makes the C++ build
  the documented golden path and validates it at the *config* layer, but the
  header rename and the component wiring are confirmed by `esphome config`, not
  by an `esphome compile`. The two refactored entity files in #18
  (`quietcool_fan.cpp`, `quietcool_button.cpp`) are deliberately host-unlinked;
  their new logic is covered by extracted pure functions and their compilation
  was checked only against throwaway stubs, not the real ESPHome headers.
- **`INSTALL.md` entity tables were written by reading the configs**, not by
  observing a running device. An entity that is misnamed or absent in practice
  would not be caught by that method.
- **The upstairs unit has no rollback image.** If a future flash of
  `0xCB03D7D3` regresses, there is no captured-good firmware to revert to on that
  board.
- **#17's residual stands.** Learn can still bind a lone foreign fan if the
  intended fan is silent for the entire window; #6 hardened the two-fans case,
  not this one.

Recommended gate before any flash: an `esphome compile` of the primary C++
config on the target platform, and a captured-good image for `0xCB03D7D3`.

---

*Compiled 2026-07-25 from the round's commits, cross-reviews, and the
integration branch at `a8862b6`. Issue numbers and `file:line` anchors are as
recorded during the round.*
