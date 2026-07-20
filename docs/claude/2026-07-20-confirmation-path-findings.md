# Confirmation-path investigation — 2026-07-20

Field investigation of the deployed downstairs SX1278 controller (`10.100.8.46`).
Three reported symptoms; two were real firmware defects, one was not a bridge
problem at all. A third defect was found during validation and is still open.

All timings below are measured from live captures, not inferred. Where an
earlier inference was wrong, that is called out explicitly — the pattern of
error is itself a finding.

---

## Summary

| # | Defect | Status |
|---|---|---|
| A | Speed→speed commands never confirmed; panel stuck showing the previous speed with `?` | **Fixed** (`4a98b08`) |
| B | An OEM-set timer made fan-state authority permanently unrecoverable | **Fixed** (`4a98b08`) |
| C | OEM remote wedges, cannot read the fan, timer buttons dead | **Not a bridge defect** |
| D | Local commands confirm via the OEM-recovery fallback instead of the closed loop | **Open** |

---

## The central measurement

**The fan emits an unsolicited state report ~1.2 s after every command it
receives. No query is required.**

Seven trials, all driven over the ESPHome native API with no OEM remote
involvement (`scratchpad/fanctl.py`):

| Command | Δ from TX request to report |
|---|---|
| HIGH (from LOW) | +1196 ms |
| LOW (from OFF) | +1216 ms |
| HIGH (from LOW) | +1212 ms |
| OFF (from HIGH) | +1198 ms |
| LOW (from OFF) | +1257 ms |
| LOW (**no-op**, already LOW) | +1185 ms |
| OFF (from LOW) | +1154 ms |

Mean **1203 ms**, range 1154–1257 ms, spread ~103 ms. Measured from the *end*
of the command burst (the anchor the firmware uses when scheduling), the report
lands at roughly **+705 to +807 ms**.

Structure: a standard 3-frame burst, frames ~100–140 ms apart, identical in form
to a query response and to an OEM remote command. There is no direction or
source bit — correlation must be by timing.

Critically, **a no-op command still produces a report** (trial 6: commanding LOW
while already LOW). So the report is a response to *receiving* a command, not to
*changing state*. This means the common path needs no query at all.

**The fan actuates at ~1.2 s.** Any query sent before that returns the
pre-command state.

---

## Defect A — speed→speed commands never confirmed

**Symptom.** Commanded HIGH from HA; the fan physically went to HIGH; the panel
kept showing `LOW?` until a manual Refresh.

**Capture** (pre-fix):

```
06:27:14.990  TX request: HIGH (0xBF)
06:27:15.602  query 1 after command 1/4
06:27:16.116  RX 'STATE 9F 9F'                    <- stale LOW
06:27:16.261  consensus state=1F raw=9F
06:27:16.261  possible OEM override: fan is LOW continuous; yielding (no re-fire)
06:27:17.013  Contradictory tail frame BF invalidated confirmed authority
```

**Root cause.** `closed_loop_query_delay_ms` was 200 ms, but the command burst
takes ~400 ms, so the query went on air at ~612 ms — *before the fan actuates at
~1.2 s*. The fan therefore answered with its pre-command state. `should_yield`
then read that stale running state as evidence of a human at the OEM remote,
cancelled every re-fire and abandoned the transaction.

**Why it looked intermittent.** Off→speed escaped it, because the stale reply
has `duration == 0` and `should_yield` requires `actual_duration != 0`. Only
speed→speed from an already-running fan hit it — and that case was 100 %
reproducible.

**Fix.** Query delay 200 → 1500 ms, plus a new `cl_prior_confirmed_state`
discriminator so the bridge never surrenders to an echo of the state it just
left. See `4a98b08`.

---

## Defect B — an OEM-set timer made authority unrecoverable

**Capture:**

```
06:36:43.605  Manual response consensus state=31 raw=B1 (2 total/2 exact)
06:36:43.619  observed HIGH timer 1h (raw B1); timer remaining unknown (state not promoted)
06:43:10.696  Manual response consensus state=31 raw=B1 (2 total/2 exact)
06:43:10.771  observed HIGH timer 1h (raw B1); timer remaining unknown (state not promoted)
```

**Root cause.** `state_authoritative` required
`actual_timer_hours == 0 || locally_anchored_timer`. A timer set on the OEM
remote has no local anchor, so it could never promote — and neither the 3 s auto
re-query nor the manual Refresh button could restore authority. The panel
asserted `OFF` over a fan running at HIGH, indefinitely.

The reasoning behind the gate was half right: a query reports *programmed* hours,
never *remaining* time, so no countdown can be derived for a timer we did not
start. But that is a statement about the countdown, not about the fan. The same
report fully determines speed and running/off.

**Fix.** Split `state_authoritative` from `timer_authoritative`. Fan state
promotes; the countdown stays honestly unknown (`NAN`). `Fan Confirmed Off`
cannot invert, because every active timer has a nonzero duration nibble.

---

## Defect C — the OEM remote wedging (NOT a bridge defect)

The investigation opened on the hypothesis that the bridge was jamming the
remote with its own query packets. **The capture refutes it.**

During an 11-second window in which the remote retried query+command ~9 times
(06:29:57.794 → 06:30:08.441), `TX Count` held at **21**. The bridge transmitted
nothing. Its single auto-query came at 06:30:11.655 — 3.2 s *after* the remote
went quiet, exactly as the holdoff intends — and got a clean reply in 500 ms.

The remote was retrying blind: it fired its command ~82 ms after its own query
triple, far too fast to have waited for the fan's ~450 ms reply. Its timer
buttons were dead, which follows directly from the OEM button flow documented at
`README.md:389-405` — query → listen → validate fan state → *then* construct the
command. A remote that cannot decode a state reply cannot build any command.

**A remote power cycle cleared it.** Cause was remote-side state. The bridge's
transmitter is ruled out.

---

## Defect D — confirmation runs through the OEM-recovery fallback (OPEN)

Found during post-flash validation of the A/B fix.

```
11:58:13.832  TX request: LOW (0x9F)
11:58:14.286  pending response after command 1/4
11:58:15.048  Valid state frame accepted: 9F 9F          <- fan's own report, +1216 ms
11:58:15.055  OEM command 9F accepted; local TX and confirmation cancelled
11:58:15.169  Command Confirmation Status >> 'superseded by OEM remote command'
11:58:18.297  OEM exchange quiet for 3 s; sending one auto status query
11:58:18.905  refreshed LOW continuous (raw 9F) from response consensus
```

**Root cause.** The 1500 ms query delay correctly waits past actuation, but it
also lands *after* the fan's unsolicited report at ~1.2 s. That report therefore
arrives with no query epoch open, is classified as external OEM traffic, and
cancels the transaction. The 3 s auto re-query then recovers the correct state.

There is **no query delay that fixes both**: too early returns a stale
pre-actuation state (defect A); too late misses the fan's free report (defect D).
The query is the wrong mechanism for post-command confirmation.

**Impact.** Correct final state, but:

- confirmation takes ~5.1 s instead of a possible ~1.2 s
- `Command Confirmation Status` reports "superseded by OEM remote command" when
  nothing external occurred
- the closed-loop verification architecture is bypassed on every local command
- the retry budget is zeroed via `refire_left = 0`

**Not** a safety regression: the misclassification only fires when the fan
*replies*, which only happens when it *received* the command. A genuinely
dropped command produces no reply, no cancellation, and the spaced re-fire
backstop proceeds normally.

**RF cost is unchanged** at 2 transmissions per command (measured: `TX Count`
11→12, 13→14, 15→16). An earlier claim in this investigation that RF usage
increased was wrong.

**Proposed fix.** Consume the fan's unsolicited post-command report as the
confirmation. Anchor a response window on the command burst end (~+705 to
+807 ms observed, so a window of roughly +500 to +1500 ms from burst end),
feed those frames into the existing consensus machinery, and reserve the `66`
query for genuine refreshes and as a fallback when no report arrives. Expected:
~1.2 s confirmation, **1 TX per command**, real closed loop restored, honest
status text.

The frame ambiguity is unchanged — a fan reply and an OEM command are
byte-identical from the same sender ID — so correlation would key off our own
command timing instead of our own query timing. That is the same class of
correlation the current design already relies on, not a new risk.

---

## Process notes

Three points worth carrying forward.

**Live captures beat code reasoning.** Three models (Codex GPT-5.6-sol, Gemini
3.1 Pro, Grok 4.5) independently analysed the source and all three concluded that
symptoms A and B were one coupled RF-collision bug caused by the bridge jamming
the remote. All three were wrong on all three counts: the symptoms were separate
defects, neither involved collision, and the bridge never transmitted during the
remote's exchange. The first log capture settled what the analyses could not.

**The post-flash gate earned its keep.** The A/B fix passed 130 structural tests,
two independent adversarial reviews (both SHIP), and three Codex implementation
rounds — and still shipped a wrong timing assumption that only real hardware
exposed. Defect D was invisible to every static check.

**The specific error was building on a perturbed measurement.** The 1500 ms delay
was derived from "the fan's new state first appears ~2.0 s after the command",
taken from a capture in which our own query at +612 ms had already disturbed the
exchange. A clean measurement with no query in flight showed the true figure is
~1.2 s. A two-minute controlled experiment would have prevented three rounds of
rework — the same mistake this document criticises the models for.
