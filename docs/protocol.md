# QuietCool RF protocol

Independently reverse-engineered from two OEM handheld STM32 firmware dumps and
live captures from real fans. The frame and state-response findings below were
confirmed on 2026-07-18; the re-entrant control behavior was characterized from
production recorder and device diagnostics on 2026-07-19.

## Radio profile

| Setting | Value |
| --- | --- |
| Carrier | 433,920,000 Hz |
| Modulation | 2-FSK (pulse shaping off) |
| Bit rate | 2,400 bit/s |
| Deviation | ±10,000 Hz |
| Preamble | Eight `AA` bytes |
| Sync word | `2D D4` |
| Packet mode | Variable-length packet engine; every confirmed protocol record has length byte `06` |
| CRC / whitening / Manchester / FEC | All disabled |
| TX power | +17 dBm (BOOST on the SX1278) |
| Repetition | Three frames, with 45 ms after each transmission |

At 2.4 kbps and ±10 kHz deviation, Carson's rule estimates an occupied width
of about 22.4 kHz. A 50 kHz SX127x FSK receive bandwidth contains that signal
while admitting about 4 dB less integrated noise than 125 kHz. This is a
protocol-derived receiver setting; it is not a claim of register-for-register
equivalence with the OEM CMT2300A's automatic filtering. TX modulation is
unchanged.

## Fixed six-byte application records

The radio supplies the preamble, sync, and one-byte packet length. The MCU
supplies a fixed six-byte application payload:

```text
AA AA AA AA AA AA AA AA | 2D D4 | 06 | <six application bytes>
```

There are three relevant application records:

```text
SS SS SS SS | CMD   CMD       remote command
SS SS SS SS | 66    66        wake/status query
CB|CE I1 I2 I3 | STATE STATE  fan response
```

- `SS SS SS SS` is the paired remote's four-byte sender ID. It begins with
  `CB`; the last three bytes are factory-specific.
- `I1 I2 I3` is that exact three-byte factory suffix. For example, one test
  installation uses sender ID `CB 12 34 56`, so its normal response begins
  `CB 12 34 56`. This is an example, not a universal ID.
- The final byte is transmitted twice. The OEM parser requires the two copies
  to be exactly equal.

The STM32 always reads seven FIFO bytes: the hardware `06` length byte plus the
six application bytes. It has no buffer space or parser for a trailing model or
capability structure. See [firmware-analysis.md](firmware-analysis.md) for the
addresses and disassembly evidence.

## Remote command byte

The handheld constructs a command as:

```text
command = (0x8 | speed_index) << 4 | duration_nibble
```

| High nibble | `speed_index` | Speed |
| --- | --- | --- |
| `8` | 0 | Off / no remembered speed |
| `9` | 1 | Low |
| `A` | 2 | Medium |
| `B` | 3 | High |

| Low nibble | Meaning |
| --- | --- |
| `0` | Off |
| `1` / `2` / `4` / `8` | 1 / 2 / 4 / 8 hours |
| `C` | 12 hours |
| `F` | Continuous on |

Examples: `9F` Low continuous, `AF` Medium continuous, `BF` High
continuous, `B1` High for one hour, `90` Off with Low remembered, and `80`
Off with no remembered speed.

Bits 4–5 hold the two-bit speed index and bits 0–3 hold the duration. Although
the handheld sets bit 7 in commands, **bit 7 is not a universal direction
marker**. Fan responses have been captured both with bit 7 clear and set;
source is established by transaction timing and exact response validation.

## Fan response validation

The OEM parser accepts a response only when all of these are true:

1. The application payload is exactly six bytes.
2. The prefix is `CB` (normal state) or `CE` (special/ack-like response).
3. All three sender-suffix bytes match the paired ID exactly.
4. The two state bytes are exactly equal.

There is no bit mask, fuzzy comparison, or error repair in the OEM parser.
Consequently captures with `CA`, `DB`, or `C3` in place of `CB`, or one-bit ID
errors such as `CB 00 46 39`, are not valid OEM frames by themselves.

`CB` is the ordinary state response. An exact `CE` response sets an internal
flag that suppresses the remote's normal state transition. No clean live `CE`
response has yet been isolated, so “special/ack-like” describes the observed
firmware behavior without assigning an unverified product-level meaning. A
conservative controller logs `CE` but does not use it to confirm state.

## State and capability byte

The response byte combines canonical state with optional model capability:

```text
canonical_state = response_state & 0x3F
speed           = (canonical_state >> 4) & 0x03
duration        = canonical_state & 0x0F
capability      = response_state >> 6
```

| Bits 7:6 | Capability behavior recovered from the OEM UI |
| --- | --- |
| `0` | No capability metadata in this report |
| `1` | One-speed model |
| `2` | Two-speed model; cycle Low ↔ High and skip Medium |
| `3` | Three-speed model; cycle Low → Medium → High |

For a running or timed request, compare the desired command's lower six bits
with `canonical_state`. For an Off request, the OEM uses an intentional
wildcard: any report with duration nibble zero confirms Off, regardless of its
remembered speed or capability bits.

**That wildcard describes the remote's state comparison, not a proven fan
acceptance requirement.** One live test saw a High-timer fan ignore six spaced
`90` Off bursts and then accept Off first-try from Low. Production later saw
`90` succeed while the last apparent state was High. The evidence therefore
does not establish that matching the remembered-speed nibble caused acceptance.

Using `90` from Low, `A0` from Medium, and `B0` from High remains an
OEM-faithful compatibility policy because the handheld preserves that nibble.
It is not a protocol requirement demonstrated by these captures. The neutral
`80` form is decoded on receive but is not the normal transmitted policy. For
control-flow equality, every `x0` form is the same semantic Off request.

Exact live responses include:

| Response | Meaning |
| --- | --- |
| `CB 12 34 56 B0 B0` | Example installation: Off, High remembered, two-speed model |
| `CB 12 34 56 90 90` | Example installation: Off, Low remembered, two-speed model |
| `CB 12 34 56 1F 1F` | Example installation: Low continuous, no capability metadata in this report |

The repeated `1F 1F` reports were previously rejected by an over-strict
`9x/Ax/Bx` whitelist. They are valid fan state reports. Conversely, the clean
`90 90` and `B0 B0` responses prove that a set bit 7 does not identify a remote
command.

## Malformed long callbacks are not rich responses

Captured application callbacks of 7, 14, 36, 38, and 63 bytes are not extended
model/capability records. For example:

```text
CB 02 34 56 90 90 02 18 49 84 C5 EB 9C A3
```

The likely errors are an on-air length byte changed from `06` to `0E` and an ID
byte changed from `12` to `02`; the SX1278 then returned the damaged first six
bytes followed by FIFO/noise. The fixed seven-byte OEM buffer and exact ID
checks prove that the handheld cannot consume a rich 14-byte reply.

Production decoding should therefore remain strict outside a locally anchored
query. A weak-link recovery tier may use the first six bytes only inside that
query epoch when the header differs from `CB` by at most one bit, is closer to
`CB` than `CE`, the duplicated state agrees, and the canonical state is valid.
The malformed callback is still not individually authoritative: require two
agreeing candidates with at least one exact frame, or three agreeing recovered
candidates, and do not count callbacks less than 60 ms apart.

## Query timing and closed-loop control

The OEM interaction is query → listen in bounded windows → command. Direct
captures put responses about 417–648 ms after the actual start of `66 66`
transmission; after a dropped command, one query's first clean report arrived
at about +805 ms. A reliable SX1278 design therefore uses:

- a locally anchored response window from 300 to 1,100 ms;
- the bounded consensus rules above;
- a separate 2,500 ms classification-only tail, because a second exact
  response train was observed as late as about 1.65 seconds; and
- exact `CE` handling that never confirms state.

The long tail does not extend the response deadline or delay a command re-fire.
It keeps late fan repeats classified as passive responses so they cannot
masquerade as a new OEM command. Because the tail endpoint is inclusive, the
next query is scheduled so its 300 ms acceptance floor starts one millisecond
after the previous tail expires; command re-fires remain eligible while that
query is pending.

A closed-loop controller layers this onto the existing spaced re-fire safety
mechanism:

1. Classify the requested semantic state before changing transaction data.
   Every equivalent request joins its active transaction without TX, counter
   reset, or clearing query/consensus evidence. Off equivalence includes every
   duration-zero `80`/`90`/`A0`/`B0` variant.
2. For a genuinely new request, arm the complete transaction and fixed attempt
   budget before enqueueing the normal three-frame command burst. The
   remembered-speed Off variant follows the compatibility policy above. Mark
   both fan and timer physical state unknown before the command can execute.
   The sender queue is bounded at five runs and may reject an over-capacity
   execution; the armed, idle-gated spaced re-fire remains the bounded path for
   the latest desired transaction once the queue drains. At actual execution,
   a stale queued state command is rejected before airtime.
3. Schedule a `66 66` query at least 200 ms after that burst actually
   completes, and later if required by the previous-tail quarantine above.
4. Compare a consensus response using lower-six-bit state and the Off wildcard.
   The requested state is not published optimistically. Only authoritative
   correlated consensus may publish confirmed state; a passively heard
   external OEM command is diagnostics-only and never mutates the safety fan
   entity.
5. Cancel remaining spaced re-fires only after a match. On mismatch or no
   consensus, leave the one-second spaced re-fire eligible; an Off mismatch may
   update the remembered-speed variant without creating a new transaction.
   A running-state mismatch that decodes as a valid remote command yields to
   the possible physical override (Off transactions never yield).
6. Stop after a fixed attempt budget: four total command bursts normally, six
   total for Off. A joined request cannot replenish that budget.
7. Publish the diagnostics `Last Confirmed Fan State`, `Command Confirmation
   Status`, `Fan Speed Capability`, `Fan State Known`, `Fan Confirmed Off`, and
   `Timer State Known`. `Fan Confirmed Off` is one atomic safety value: true
   only for authoritative Off consensus, false for running or unknown.

The implementation must keep all transaction globals volatile, retain
`restore_mode: NO_RESTORE`, refuse transmission without a learned ID, and route
every packet through one serialized send point. Boot, OTA, API reconnect,
learning, and received frames must never initiate transmission. An exact
external OEM query/command takes priority over queued local work; a heard query
reserves a two-second no-local-TX holdoff for the complete physical exchange.

### State-knowledge boundary

The custom fan platform separates command requests from observations, but
ESPHome's native Fan API still cannot encode missing state. On initial HA
subscription it exposes the fan object's raw defaults—Off and the pre-seeded
Low speed—even though no RF evidence exists. `Fan State Known` is therefore the
machine-readable authority diagnostic for whether the fan entity is physical
truth. `Timer State Known` performs the same role
for the timer select and countdown, which are not initialized to a guessed
`None` at boot. HA interlocks should use the single `Fan Confirmed Off` sensor
rather than separately joining fan state and `Fan State Known`, since those
entities can be delivered in different API batches.

Every genuinely new state/timer command and every actually executed non-query
burst, including a spaced re-fire, invalidates both known flags. Only a response
correlated to a local query or closed-loop consensus is eligible to restore
them; the ambiguity and timer rules below can deliberately keep authority
false. Passive OEM traffic is diagnostics-only because hearing a command does
not prove receiver actuation.

Outgoing commands never optimistically arm or clear confirmed timer metadata.
If all responses are lost, timer state remains unknown and an expiry cannot
publish guessed Off. The reported duration is programmed hours, not remaining
time, so only confirmation of a locally initiated timer command can anchor a
trusted countdown. A manual-query active-timer report has unknown age and is
not promoted to authoritative fan/timer state. Estimated expiry invalidates
authority and never publishes Off because the fan emits no RF expiry frame.

Authority is invalidated for every RF action the controller actually observes,
but this is not a continuous motor sensor. If all frames from a later OEM
exchange are missed, the last RF confirmation can become stale. Safety systems
that require fresh physical truth need an explicit Refresh policy or an
independent airflow/motor sensor.

### Authority, manual-query, and burst-ordering refinements

Command and response records share the same six-byte wire shape. Therefore a
command-shaped mismatching consensus (`9x`/`Ax`/`Bx`, or `80`) inside a local
query window is ambiguous: it may be a fan report, or an OEM press whose query
was missed while this half-duplex controller was transmitting. It never gains
state authority. An On transaction yields to a possible running OEM override;
an Off transaction does not yield and continues its fixed safety budget. A
matching consensus remains authoritative because it proves the requested
outcome regardless of source ambiguity.

Manual Refresh and the raw probe use the same consensus threshold as command
confirmation. Their response window is anchored to actual serialized query
start, not button/enqueue time, and a manual query is rejected while a command
transaction, completed response mailbox, or any prior query's classification
epoch is active. A completed report is consumed once; later same-state repeats
remain classification-only, while a contradictory tail report invalidates
authority and poisons an unconsumed mailbox. A new command poisons any older
manual epoch. An Off observation is also kept unknown while a future energizing
retry remains armed.

The OEM sends three frames per press. Live callbacks are about 102 ms apart, so
normal RX uses a fixed 300 ms duplicate epoch measured from the first accepted
frame. Suppressed repeats do not slide the timestamp. This accepts a distinct
same-command physical press after the fixed epoch instead of chaining it into a
450 ms sliding window and potentially hiding it from physical-control priority.

Selecting `None` in the timer UI sends no RF. The protocol has no proven
non-actuating timer-clear command; selecting a speed explicitly sends
continuous mode.

### 2026-07-19 production RCA

The earlier attempt ceiling was bounded only within one wrapper invocation.
Production exposed that boundary when a Home Assistant window interlock
re-entered the Off path: the `TemplateFan` first published optimistic Off, a
later `B1` confirmation restored On, and the interlock invoked Off again. Each
invocation replaced the active transaction instead of joining it.

Observed recorder and device evidence:

- 107 fan entity transitions in 73.34 seconds;
- 54 interlock executions;
- 53 High/`B1` mismatches, every one still reporting five attempts left; and
- 118 transmitted three-frame bursts, or 354 application frames.

No ESP-originated On command appeared. The evidence supports a control-loop
reset, not 53 proven physical motor cycles. Separating local requests from
observed publication prevents the false state edge; passive OEM traffic remains
diagnostics-only. Semantic active-Off coalescing independently keeps the
RF transaction bounded if HA repeats the request for any reason. The spaced
re-fire cadence is preserved.

### Repository implementation status

The query decoder and single-transaction closed loop were compiled, OTA-tested,
and verified in the downstream working configuration from which this research
was derived. The final non-energizing Off test confirmed after one command and
one query, reporting `OFF (raw 90)`, `2-speed`, and
`confirmed OFF (raw 90) after 1 command(s), 1 query(s)`. Late exact repeats at
+1,391, +1,497, and +1,590 ms remained passive and did not trigger
transmission. The flashed artifact's SHA-256 was
`674ea45beb02e14d27eebd7bdfcfdddd1c7c74acf050238fa192f0e744237689`.

That 2026-07-18 validation did not exercise re-entrant equivalent requests and
therefore did not prove transaction-scoped boundedness. The 2026-07-19 RCA was
the corrective evidence for confirmation-driven publication and semantic Off
coalescing.

The corrected build was OTA-flashed exactly once to the downstream downstairs
SX1278 controller on 2026-07-19. It made no transmission during 62 idle seconds
after boot and left fan/timer authority unknown. A manual Refresh used one query
and accepted two exact `90 90` replies at about +396 and +520 ms. Three rapid HA
Off calls then produced one transaction, with the two repeats logged as joins
rather than budget resets. Exactly one Off burst and one automatic query were
sent, and `90` confirmed after one command and one query. Two late exact reports
at about +1,422 and +1,520 ms remained passive. Total TX Count was three across
the manual Refresh and Off test, while HA recorded no fan or interlock state
transitions. The final HA fan state was Off and the interlock remained enabled.
Without an independent motor/airflow sensor, this proves RF/entity behavior but
not physical motor state.

Both public YAML templates share the corrected transaction logic:
`quietcool-lora32.yaml` on SX1278 and `quietcool-lora-v3.yaml` on SX1262. The
SX1262 uses a 58.6 kHz receive bandwidth, the nearest FSK-legal value to the
validated SX1278 50 kHz setting. ESPHome 2026.7.0 config validation and
compilation succeeded for both public targets and both downstream wrappers; the
public compile hashes were `0x80f65068` (SX1278) and `0x0be208d7` (SX1262).
The live downstream SX1278 wrapper hash was `0xef85b7d8`, and its flashed binary
SHA-256 was
`714c455a1673c3c3255132df84f68d020afbbcfd989594c34c997a940cafc59d`.
The public named artifacts were not flashed. The second (upstairs) unit was offline
and not flashed, and the V3 board still awaits hardware bring-up.
