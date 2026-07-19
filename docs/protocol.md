# QuietCool RF protocol

Independently reverse-engineered from two OEM handheld STM32 firmware dumps and
live captures from real fans. The frame and state-response findings below were
confirmed on 2026-07-18.

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

The long tail must not extend the response deadline or delay a retry. It only
keeps late fan repeats classified as passive responses so they cannot masquerade
as a new OEM command.

A validated closed-loop controller layers this onto the existing spaced
re-fire safety mechanism:

1. Record the desired state and send the normal three-frame command burst.
2. Schedule a `66 66` query 200 ms after that burst actually completes.
3. Compare a consensus response using lower-six-bit state and the Off wildcard.
4. Cancel remaining spaced re-fires only after a match. On mismatch or no
   consensus, leave the one-second spaced re-fire eligible.
5. Stop after a fixed attempt budget: four total command bursts normally, six
   total for Off.
6. Publish the diagnostics `Last Confirmed Fan State`, `Command Confirmation
   Status`, and `Fan Speed Capability`.

The implementation must keep all transaction globals volatile, retain
`restore_mode: NO_RESTORE`, refuse transmission without a learned ID, and route
every packet through one serialized send point. Boot, OTA, API reconnect,
learning, and received frames must never initiate transmission. An exact
external OEM query/command takes priority over queued local work.

### Repository implementation status

The closed-loop algorithm above was implemented, compiled, OTA-tested, and
verified in the downstream working configuration from which this research was
derived. The final non-energizing Off test confirmed after one command and one
query, reporting `OFF (raw 90)`, `2-speed`, and
`confirmed OFF (raw 90) after 1 command(s), 1 query(s)`. Late exact repeats at
+1,391, +1,497, and +1,590 ms remained passive and did not trigger
transmission. The flashed artifact's SHA-256 was
`674ea45beb02e14d27eebd7bdfcfdddd1c7c74acf050238fa192f0e744237689`.

Both public YAML templates in this repository now ship that closed-loop
implementation: `quietcool-lora32.yaml` carries the same source the live
SX1278 validation ran (with an anonymized, unprovisioned sender seed), and
`quietcool-lora-v3.yaml` carries the identical logic on the SX1262 with a
58.6 kHz receive bandwidth (the nearest FSK-legal value to the validated
50 kHz). The V3 board has not yet had hardware bring-up, so its closed loop
compiles and validates but is not yet bench-verified on air.
