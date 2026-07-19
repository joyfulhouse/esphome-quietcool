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
late fan repeats from being misclassified as a new OEM command.

The former SX1278 setting admitted 125 kHz. At this protocol's 2.4 kbps and
±10 kHz deviation, Carson's estimate is about 22.4 kHz. An explicit 50 kHz
receiver bandwidth contains the signal while reducing integrated noise by
about 4 dB. That is a conservative SX127x setting, not proof that its filter is
identical to the CMT2300A's automatic filter.

## Validated closed-loop design

The downstream implementation layers query confirmation on top of the existing
one-second spaced re-fire safety mechanism:

1. A command wrapper records the desired state and retains the original retry
   budget: four total command bursts normally and six total for Off.
2. The sole serialized sender transmits three copies. Two hundred milliseconds
   after actual burst completion, it sends a `66 66` query through that same
   send point.
3. The actual query start anchors the 300–1,100 ms consensus window and 2.5 s
   passive classification tail.
4. Exact OEM frames and the bounded recovery tier feed consensus. State is
   compared as lower six bits, with the OEM duration-zero Off wildcard. `CE`
   never confirms state.
5. Confirmation publishes `Last Confirmed Fan State`, `Command Confirmation
   Status`, and `Fan Speed Capability`, then cancels remaining spaced re-fires.
   A mismatch or no consensus leaves the existing retry eligible; exhausting
   the fixed budget publishes failure and stops.
6. An exact external OEM query/command supersedes queued local work. Late
   response repeats cannot masquerade as that external traffic.

Safety invariants remain essential: volatile transaction globals,
`restore_mode: NO_RESTORE`, no transmission at boot/OTA/API reconnect/from RX,
no sending before provisioning, and exactly one serialized radio-send site.
Learning changes receive/storage state only. Rapid same-command presses consume
the same fixed attempt budget instead of creating unbounded retries.

### Live validation and repository status

The design above passed 96 structural/unit tests, ESPHome configuration
validation, compilation, and two non-energizing live Off transactions. The
final transaction confirmed `90 90` after one command and one query. The three
diagnostics reported `OFF (raw 90)`,
`confirmed OFF (raw 90) after 1 command(s), 1 query(s)`, and `2-speed`. Late
exact repeats at query ages +1,391, +1,497, and +1,590 ms remained passive, did
not change confirmation, and did not transmit. The device remained Off and the
window interlock stayed enabled. The final flashed artifact had SHA-256:

```text
674ea45beb02e14d27eebd7bdfcfdddd1c7c74acf050238fa192f0e744237689
```

This validation occurred in the downstream working configuration used for the
2026-07-18 research. The public YAML templates currently in this repository are
still command/passive-RX implementations and **do not yet contain this
closed-loop controller**. V3 is likewise passive RX, not a query/confirmation
loop. This section records the proven design for a future public source port.

## Analysis tooling

Disassembly used Capstone in Thumb/Cortex-M0+ mode. Scratchpad scripts resolved
PC-relative literals around `0x08005F90`, traced cross-references, compared both
firmware images, and audited generated ESPHome send sites. The scripts and raw
copyrighted firmware images are intentionally not distributed here.
