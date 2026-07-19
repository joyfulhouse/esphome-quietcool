# Firmware reverse-engineering

How the QuietCool protocol and state-response path were recovered. The carrier
and 2-FSK modulation were established from SDR captures; packet construction,
response validation, capability encoding, and the query loop were then derived
from two OEM handheld firmware dumps and checked against live fan traffic.

> The raw OEM firmware images are **not** redistributed in this repository (they
> are QuietCool's copyrighted code). Only independently derived facts,
> constants, offsets, and behavior are documented here.

## The remote's MCU

- **STM32G030K6T6** (Cortex-M0+, 32 KiB flash), read-protection **RDP Level 0**
  (unprotected), so flash can be read over SWD.
- Dump command: `st-flash read fw.bin 0x08000000 0x8000` with an ST-Link
  (macOS may require `sudo` for USB access).

## Flash memory map (32 KiB)

| Region | Offset | Contents |
| --- | --- | --- |
| Vector table | `0x0000–0x00C0` | Cortex-M0+ handlers |
| Application | `0x00C0–0x6E00` | Main code |
| Button table | `0x6E00–0x6E20` | Button indices `01 02 03 04 06 07 08 09` |
| Device config | `0x6E20–0x6E50` | Sender-ID template + packet formats |
| RF register config | `0x6E50–0x6FD0` | CMT2300A-compatible register values |
| Erased padding | `0x7000–0x7800` | `FF` |
| Runtime ID suffix | `0x7800–0x7802` | Per-unit ID bytes, little-endian |

## Sender-ID mechanism

The compiled packet template carries `CB 00 01 52`. At boot, firmware replaces
the final three bytes with the per-unit values at flash `0x7800` (stored
little-endian), producing the four-byte runtime sender ID. `CB` is the common
prefix; the other three bytes are installation-specific.

Two remotes bought together were compared byte-for-byte:

| | Remote A | Remote B |
| --- | --- | --- |
| Size | 32,768 bytes | 32,768 bytes |
| SHA-256 | `418dedb11ce47c5d07efa886e34273474552983e8699498923b563163df66cef` | `1163edca4898b24f30c16e94eb23f262da7bd347ce1d5146c9cc2c64073cec00` |

They differ in exactly three bytes, all at `0x7800–0x7802`. Code, button
table, packet templates, and RF configuration are identical. There is no
separate one-/two-/three-speed remote firmware image; the fan reports that
capability at runtime.

### Learn mode confirms the over-the-air ID

The public controller's learn mode captured the four-byte sender ID from two
remote presses, byte-for-byte matching the remote's factory suffix. Owners do
not need a firmware dump to provision a fan; the dumps only confirmed what the
over-the-air learner records.

## Command construction and decoding

The response decoder at `0x08005F90` reads duplicated bytes at response offsets
5 and 6. Its speed extraction includes this sequence:

```asm
0x0800602C: ldrb r3, [r3, #4]
0x0800602E: lsls r3, r3, #2
0x08006030: uxtb r2, r3
```

The effective command/state fields are:

```text
speed    = (command >> 4) & 0x03
duration = command & 0x0F
```

The handheld command constructor sets bit 7 and emits high nibbles
`8`/`9`/`A`/`B`. The decoder masks to the two speed bits. This explains why
both `80` and `90` are valid Off commands: their duration nibble is zero, while
their remembered speed differs.

This does **not** make bit 7 a direction marker. The fan has returned valid
state reports with bit 7 both clear (`1F`) and set (`90`, `B0`). Direction is
known from a response's exact structure and its position inside the bounded
listen epoch after a local query.

## OEM per-press query path

The relevant functions establish a real request/response transaction:

1. `0x08005EFC` transmits the `66 66` query as a three-frame burst.
2. `0x08005DE8` switches the radio to receive mode.
3. `0x08005F90` validates and parses the response inside a bounded poll loop.
4. `0x08005E14` later transmits the actual user command.

The poll counter at `0x20000323` is capped at `0x63`. Timed steps make the
listen/retry cadence roughly 0.56 seconds. An earlier analysis mislabeled
`0x0800301C` as a timeout routine; disassembly shows that it samples GPIO PA8.

The important reliability property is not unbounded passive reception. The
handheld transmits a query, immediately listens during known response timing,
and does so near the fan.

## Fixed receive buffer proves the response length

The OEM receive path has a fixed seven-byte FIFO record:

- initialization at `0x08005D48` writes `1` to `0x200002C9`;
- `0x08005D4E` writes receive count `7` to `0x200002CC`;
- `0x08004954–0x08004968` calls `0x08006C5C(destination, 7)`;
- the response buffer is `0x2000031C–0x20000322`, and the poll counter starts
  immediately afterward at `0x20000323`; and
- `0x08005F90` only accesses buffer offsets 1 through 6.

Buffer offset 0 is the packet engine's `06` variable-length prefix. The
application payload is therefore always exactly six bytes:

```text
CB|CE  id1 id2 id3  state state
```

There is no trailing model/capability field, no parser loop over additional
bytes, and no room before the next global for one.

## Exact response validation and prefix semantics

The STM32 requires:

- prefix `CB` or `CE`;
- an exact match of all three runtime sender-suffix bytes; and
- exact equality of the duplicated state bytes.

The parser has no masked ID comparison, Hamming tolerance, or error-repair
path. Prefix variants `CA`, `DB`, and `C3`, and one-bit suffix variants such as
`CB 00 46 39`, `CB 00 47 31`, `CB 00 47 21`, and `CB 00 47 29`, are not
OEM-valid frames individually.

`CB` is the ordinary state branch. An exact `CE` plus exact suffix and duplicate
sets the flag at `0x2000022F`; the branch at `0x080063D6–0x080063E8` then
suppresses the normal state transition. Because no clean `CE` response has been
isolated live, “special/ack-like” or “no state transition” is more precise than
assigning it an undocumented product name. A conservative controller logs an
exact `CE` but excludes it from state confirmation.

## Lower-six-bit state and upper-two-bit capability

The OEM matcher canonicalizes the response with `state & 0x3F`. A running or
timed command must match that lower-six-bit value. For Off, it intentionally
accepts any reported value whose duration nibble is zero, regardless of
remembered speed.

Separately, code near `0x080063FA` stores `state >> 6` at `0x20000013`. UI
handlers at `0x080056E6–0x0800577E` interpret it as:

| `state >> 6` | UI behavior |
| --- | --- |
| `0` | No capability metadata in this report |
| `1` | One-speed fan |
| `2` | Two-speed fan: cycle speed 1 ↔ 3, skipping Medium |
| `3` | Three-speed fan: cycle 1 → 2 → 3 |

The complete decode is therefore:

```text
canonical_state = state & 0x3F
speed           = (canonical_state >> 4) & 0x03
duration        = canonical_state & 0x0F
capability      = state >> 6
```

This upper-field interpretation explains the OEM remote graying/skipping
Medium for a two-speed model without any extended response payload.

## Live captures reconcile the parser with the air

Fresh, raw query captures from one labeled test installation (sender ID
`CB 12 34 56`) produced:

| Context | Exact application payload | Decode |
| --- | --- | --- |
| Direct query, about +419/+524 ms | `CB 12 34 56 B0 B0` | Off, High remembered, capability `2` (two-speed) |
| Query while Low after a dropped Off, repeated ten times | `CB 12 34 56 1F 1F` | Low continuous, no capability metadata in this report |
| Post-OTA direct query, about +421/+522/+621 ms | `CB 12 34 56 90 90` | Off, Low remembered, capability `2` (two-speed) |

The `1F 1F` reports were genuine frames that the former `9x/Ax/Bx` whitelist
incorrectly rejected. The exact length, sender suffix, and duplicate match the
OEM rules. The `90 90` and `B0 B0` reports establish that set bit 7 is also
valid in fan responses.

### Off-variant evidence boundary

The OEM constructor preserves remembered speed in Off commands (`90`/`A0`/`B0`),
so following that pattern is a conservative compatibility policy. It is not
proven necessary for command acceptance. One test saw six `90` bursts ignored
while the fan reported a High timer, but the 2026-07-19 production incident
later ended when `90` succeeded from an apparent High state. RF delivery,
receiver timing, and the speed nibble were not independently isolated. All
duration-zero variants must therefore compare as one semantic Off transaction,
even if the transmitted byte retains the OEM-style speed nibble.

### Why the long callbacks are malformed

One captured callback contained 14 application bytes:

```text
CB 02 34 56 90 90 02 18 49 84 C5 EB 9C A3
```

It is not a rich response. The likely corruption changed the on-air packet
length from `06` to `0E` and sender byte `12` to `02`; the SX1278 consequently
delivered a damaged six-byte prefix plus FIFO/noise. Other observed callback
lengths of 7, 36, 38, and 63 are the same class. The OEM's fixed seven-byte
read and exact ID comparison cannot accept them as received.

A normal decoder should keep exact six-byte validation. A deliberately bounded
weak-link recovery tier can consider the first six bytes only during a locally
anchored query epoch, only with at most one header bit error (closer to `CB`
than `CE`), matching duplicate state, and valid canonical state. Consensus then
requires two agreeing candidates with at least one exact frame, or three
agreeing recovered candidates; candidates less than 60 ms apart do not count
separately. This is consensus recovery, not a claim that a malformed callback
is individually valid.

## Query timing and receiver bandwidth

Direct live responses arrived about 417–648 ms after actual query start. After a
dropped command, one query's first clean report arrived at about +805 ms. A
downstream SX1278 implementation therefore validated a 300–1,100 ms
confirmation window. It also found a second exact response train extending to
about 1.65 seconds, so it retains a separate 2.5-second classification-only
tail. The tail does not extend the confirmation deadline; it merely prevents
late fan repeats from being misclassified as a new OEM command. Because the tail
endpoint is inclusive, a following query's 300 ms acceptance floor is placed one
millisecond after it expires. Command re-fires remain eligible on their original
one-second spacing while that query is pending, so observed late repeats cannot
be counted as fresh post-retry evidence.

The former SX1278 setting admitted 125 kHz. At this protocol's 2.4 kbps and
±10 kHz deviation, Carson's estimate is about 22.4 kHz. An explicit 50 kHz
receiver bandwidth contains the signal while reducing integrated noise by
about 4 dB. That is a conservative SX127x setting, not proof that its filter is
identical to the CMT2300A's automatic filter.

## Corrected closed-loop design

The corrected design layers query confirmation on top of the existing
one-second spaced re-fire safety mechanism:

1. Before any mutation or TX, compare the requested semantic state with the
   active transaction. Every equivalent request (including any duration-zero
   variant for Off) joins its active
   transaction; it does not transmit or reset attempts, queries, consensus,
   timer state, or status.
2. A genuinely new request records the desired state and fixed budget before
   enqueue: four total command bursts normally and six total for Off. It marks
   both fan and timer physical state unknown before any possible transmission.
3. The sole serialized sender transmits three copies. Two hundred milliseconds
   after actual burst completion, it sends a `66 66` query through that same
   send point. The queue is bounded at five runs; an over-capacity execution can
   be rejected, so the armed, idle-gated re-fire remains the convergence path
   for the latest desired transaction. Actual execution rejects a queued state
   command that no longer equals the latest desired command.
4. The actual query start anchors the 300–1,100 ms consensus window and 2.5 s
   passive classification tail.
5. Exact OEM frames and the bounded recovery tier feed consensus. State is
   compared as lower six bits, with the OEM duration-zero Off wildcard. `CE`
   never confirms state.
6. Authoritative RF consensus publishes the fan entity, `Last Confirmed Fan
   State`, `Command Confirmation Status`, and `Fan Speed Capability`; a
   requested state is never published optimistically. It sets `Fan State
   Known` and the atomic `Fan Confirmed Off` diagnostic, cancels remaining
   spaced re-fires, and makes a timer countdown authoritative only when it
   confirms this controller's locally initiated timer command. A mismatch or no
   consensus leaves the existing retry eligible and both known flags false;
   exhausting the fixed budget publishes failure and stops.
7. An exact external OEM query/command supersedes queued local work. A heard
   OEM query reserves a two-second no-local-TX holdoff for the physical
   query/response/command exchange. Passive
   OEM traffic remains diagnostics-only and never mutates the safety fan
   entity. Late response repeats cannot masquerade as that external traffic.

Safety invariants remain essential: volatile transaction globals,
`restore_mode: NO_RESTORE`, no transmission at boot/OTA/API reconnect/from RX,
no sending before provisioning, and exactly one serialized radio-send site.
Learning changes receive/storage state only. Boot does not send even a status
query. Rapid equivalent calls of every command type join the same fixed attempt
budget instead of starting or replenishing transactions.

The explicit known flags are required even with the custom confirmation-driven
fan platform. ESPHome's native Fan API has no representation for missing state,
so its first HA subscription exposes raw object defaults (Off with Low as the
pre-seeded speed) even though `control()` never published them. Safety logic must
therefore not treat the fan entity alone as physical evidence. `Fan State
Known` records entity authority, while the single `Fan Confirmed Off` diagnostic
is the safe HA interlock input because it avoids cross-entity API ordering.
`Timer State Known` similarly gates timer select/countdown use; the select is
not initialized to a guessed `None` at boot.

Every genuinely new state/timer request and every actually executed non-query
burst—including an automatic re-fire—invalidates both known flags. Outgoing
commands do not optimistically arm or clear confirmed timer metadata. Failure
or no response leaves timer state unknown, so its expiry path cannot publish a
guessed Off. The fan reports programmed timer hours rather than remaining time,
so only confirmation of a locally initiated timer command anchors the local
countdown. A manual query that finds an active timer has unknown age and cannot
authorize state/timer publication. Estimated expiry invalidates authority and
does not publish Off.

Because OEM commands and fan reports share a wire shape, a command-shaped
mismatching consensus inside a local query window is ambiguous and never gains
state authority. An On transaction may yield to a possible running physical
override; an Off transaction never yields and continues its fixed safety
budget. Manual Refresh/probe uses the same response consensus as the command
loop, with timing anchored to actual query transmission. Completed reports are
consumed once, and contradictory tail traffic poisons an unconsumed mailbox as
well as invalidating authority. A new command poisons any older manual epoch.
An otherwise valid Off report also remains unknown while an energizing retry is
still pending, so `Fan Confirmed Off` cannot turn true immediately before TX.

Normal same-command burst deduplication is a fixed 300 ms epoch from the first
accepted frame. Suppressed ~102 ms repeats do not slide it, so a distinct later
physical press cannot be hidden by the predecessor's 450 ms sliding window.
The Timer select's `None` choice is a safe no-op: the protocol has no proven
non-actuating clear command, so continuous mode requires an explicit speed
selection.

### 2026-07-19 production correction

The previous implementation satisfied its attempt ceiling only within one
command-wrapper invocation. In production, the ESPHome `TemplateFan` published
Off as soon as Home Assistant requested it. When a query reported `B1` (High,
one-hour timer), the entity returned to On and a window interlock requested Off
again, replacing the active transaction and restoring its full budget.

Recorder and device diagnostics observed 107 entity transitions in 73.34
seconds, 54 interlock runs, 53 mismatches that all still said five attempts
remained, and 118 three-frame bursts (354 application frames). There was no
ESP-originated On command. This directly supports re-entrant transaction reset
as the cause of the RF storm. It does not establish 53 physical motor cycles;
the receiver kept reporting High until the final Off confirmation.

The corrective invariants are transaction-scoped: local requests are never
published as observed state, passive OEM traffic is diagnostics-only, and
all `x0` Off variants coalesce when an Off transaction is active. Coalescing is
observation-only and preserves the existing spaced re-fire schedule and
terminal budget.

### Live validation and repository status

The predecessor design passed its then-current structural/unit tests, ESPHome
configuration validation, compilation, and two non-energizing live Off
transactions. The final transaction confirmed `90 90` after one command and
one query. The three diagnostics reported `OFF (raw 90)`,
`confirmed OFF (raw 90) after 1 command(s), 1 query(s)`, and `2-speed`. Late
exact repeats at query ages +1,391, +1,497, and +1,590 ms remained passive, did
not change confirmation, and did not transmit. The device remained Off and the
window interlock stayed enabled. The final flashed artifact had SHA-256:

```text
674ea45beb02e14d27eebd7bdfcfdddd1c7c74acf050238fa192f0e744237689
```

That artifact validated decoding and one transaction at a time; it did not
exercise re-entrant equivalent Off calls and is not evidence for the corrective
coalescing behavior.

Fresh post-correction validation ran on 2026-07-19. Final ESPHome 2026.7.0 config
validation and compilation succeeded for both public targets and both downstream
SX1278 wrappers. Public compile hashes were `0x80f65068` for SX1278 and
`0x0be208d7` for SX1262. The live downstairs wrapper built as `0xef85b7d8` at
14:25:44 PDT. Its binary SHA-256 was:

```text
714c455a1673c3c3255132df84f68d020afbbcfd989594c34c997a940cafc59d
```

That downstream artifact was OTA-flashed exactly once to the downstairs controller at about
14:26 PDT. During 62 idle seconds after boot, `TX Count` remained zero,
`Fan State Known`, `Timer State Known`, and `Fan Confirmed Off` remained false,
the last confirmed state remained unknown, and confirmation status remained
idle. A manual Refresh then consumed one TX burst and accepted two exact `90
90` replies at about +396 and +520 ms, establishing Off and two-speed
capability.

Three rapid HA Off calls then exercised the production failure mode directly.
They created one transaction; both later calls were logged as duplicate joins
at 0 of 6 completed commands with five re-fires left. They did not reset or
replenish the budget.
Only one Off burst and one automatic query followed, and the transaction
confirmed `90` after one command and one query. `TX Count` ended at three for
the complete manual-Refresh-plus-Off test. Exact late reports at about +1,422
and +1,520 ms remained passive. Home Assistant history showed no fan or
interlock state transitions during the test; the fan entity ended Off and the
window interlock remained enabled. These results validate RF consensus,
publication, and duplicate coalescing, but without an independent motor or
airflow sensor they do not prove physical motor state.

That validation occurred in the downstream working configuration used for the
2026-07-18 research. Both public YAML templates now share the corrected
transaction logic: `quietcool-lora32.yaml` on SX1278 and
`quietcool-lora-v3.yaml` on SX1262. The latter uses a 58.6 kHz RX bandwidth,
the nearest FSK-legal value to the validated SX1278 50 kHz setting, and still
awaits hardware bring-up. The rollout used a downstream wrapper, not either
public named artifact. The second (upstairs) unit was offline and was not flashed;
the V3 build was compiled but has not been tested on hardware.

## Analysis tooling

Disassembly used Capstone in Thumb/Cortex-M0+ mode. Scratchpad scripts resolved
PC-relative literals around `0x08005F90`, traced cross-references, compared both
firmware images, and audited generated ESPHome send sites. The scripts and raw
copyrighted firmware images are intentionally not distributed here.
