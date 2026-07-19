# esphome-quietcool

ESPHome firmware that controls **QuietCool whole-house / gable attic fans** over
their native 433.92 MHz radio link, from Home Assistant — no cloud, no OEM hub,
no BLE. The RF protocol was **reverse-engineered from the OEM handheld remote's
firmware** (an STM32 dump + SDR captures); this repository is an independent,
clean-room implementation of what that analysis found.

> QuietCool's wireless wall/handheld controls speak a proprietary 2-FSK protocol
> that ordinary 433 MHz gear (Sonoff RF Bridge, OOK/ASK bridges) can't reproduce.
> This project drives the fan directly with a Semtech LoRa transceiver in raw FSK
> packet mode.

## Features

- **Closed-loop confirmation** — after every command the controller sends the
  OEM's own `66 66` status query, decodes the fan's reply, and **verifies the
  fan actually did it**. Confirmed state, confirmation status, and the fan's
  reported speed capability (2-speed vs 3-speed) are all published to Home
  Assistant. A lost command is retried on a bounded, spaced schedule until the
  fan confirms — or the failure is reported explicitly instead of guessed at.
- **Direct RF fan control** — Off / Low / Medium / High (where supported by
  the fan model) on the fan entity, plus a speed-aware timer select covering
  the fan's full 1 / 2 / 4 / 8 / 12-hour range — more than the OEM remote's
  three timer buttons expose — transmitted as the exact OEM frames. The
  select syncs both ways: start a timer from the physical remote and it
  shows up selected in Home Assistant.
- **Learn mode** — capture your fan's 4-byte sender ID by pressing its OEM remote
  twice. No packet sniffing or firmware extraction needed to onboard; the ID is
  persisted in NVS and survives reboots and OTA. See
  [Learn mode](#learn-mode--porting-to-your-own-fan).
- **Home Assistant native API** — a proper `fan` entity plus diagnostics
  (TX/RX counters, last command, learned sender ID, battery voltage/level).
- **Bi-directional** — the controller also *listens*: press the OEM remote and
  the Home Assistant entity updates to match within a second. Received frames
  are strictly validated and mirrored into the entity **without ever
  re-transmitting** (no RF echo, no feedback loop).
- **On-device OLED** — animated fan icon, HH:MM:SS timer countdown, three
  HA-relayed temperatures (indoor / outdoor / attic) with semantic icons, and a
  WiFi / API / battery status row. Temperature sources are configurable from the
  HA UI, not hard-coded.
- **Safety-first** — never transmits at boot, after OTA, on API reconnect, from
  restored state, or from a received frame. Multi-model adversarially reviewed.
- **Multi-board & multi-fan** — one shared config, thin per-device wrappers.

### How the closed loop works

The fan answers the OEM `66 66` status query with a six-byte state report
(`CB` + your remote's ID suffix + a duplicated state byte). This firmware uses
the exact validation rules recovered from the OEM remote's STM32: state is
compared on the lower six bits, any zero-duration report confirms Off
regardless of remembered speed, and bits 7:6 carry the fan's speed-capability
metadata. After each command burst the controller queries, requires **response
consensus** (repeated agreeing reports inside a bounded listen window — with a
deliberately narrow recovery tier for weak-link bit errors, since the fan has
no CRC), and then either confirms and stops, or lets the pre-existing spaced
re-fire backstop continue up to its fixed attempt budget. Every outcome —
`confirmed`, `mismatch`, `no consensus`, `FAILED`, or `superseded by OEM
remote` — is published to Home Assistant, and a physical OEM remote press
always takes priority over pending automatic work. Validated live on an
SX1278 installation, where the capability diagnostics also identified the test
fan as a two-speed model. Full detail in [docs/protocol.md](docs/protocol.md)
and [docs/firmware-analysis.md](docs/firmware-analysis.md).

## Supported hardware

| Board | Radio | MCU | Config | Status |
| --- | --- | --- | --- | --- |
| LilyGO TTGO LoRa32 **V2.1** (433 MHz) | SX1278 (SX127x) | ESP32 | `quietcool-lora32.yaml` | Verified on real fans |
| Heltec / HiLetgo ESP32 LoRa **V3** (433–510 MHz) | SX1262 (SX126x) | ESP32-S3 | `quietcool-lora-v3.yaml` | Builds; awaiting hardware bring-up |

The V3 port reproduces the identical 2-FSK profile on the SX1262 (ESPHome's
`sx126x` component exposes the same bitrate/deviation/sync/preamble/variable-length
knobs). It compiles clean but hasn't been run on real hardware yet — a few pins
(status-LED polarity, the VBAT ADC divider, and the RX filter bandwidth) are
noted inline as `PIN CONFIDENCE` items to confirm on first bring-up. See
[docs/hardware.md](docs/hardware.md).

Both need a **433 MHz antenna** connected before transmitting.

### Where to buy

- **LilyGO TTGO LoRa32 V2.1 (433 MHz)** — the reference board this was built and
  verified on: <https://amzn.to/4vBvqOU>
- **HiLetgo ESP32 LoRa V3 (SX1262, 0.96" OLED, 433–510 MHz antenna)**:
  <https://amzn.to/4wagWqi>

<sub>Disclosure: as an Amazon Associate (store `joyfulhousegi-20`) the maintainers
may earn from qualifying purchases through the links above. They cost you nothing
extra.</sub>

## Quick start

```bash
# 1. Install ESPHome (uv recommended)
uv venv .venv && uv pip install --python .venv/bin/python esphome

# 2. Provide secrets
cp secrets.yaml.example secrets.yaml   # then edit

# 3. Validate, build, flash (USB first time, OTA after)
.venv/bin/esphome run quietcool-lora32.yaml
```

Then adopt the device in Home Assistant (ESPHome integration) and teach it your
fan via [Learn mode](#learn-mode--porting-to-your-own-fan). The full
step-by-step walkthrough — flashing, HA adoption, pairing, display setup,
troubleshooting — is in **[INSTALL.md](INSTALL.md)**.

## Documentation

- [INSTALL.md](INSTALL.md) — step-by-step install, pairing, and troubleshooting
- [docs/protocol.md](docs/protocol.md) — RF profile, frame format, command byte
- [docs/firmware-analysis.md](docs/firmware-analysis.md) — the reverse-engineering:
  memory map, register config, command-byte and response-parser disassembly,
  per-unit ID mechanism
- [docs/hardware.md](docs/hardware.md) — boards, wiring, antenna, buying links
- [docs/display.md](docs/display.md) — OLED layout, icon language, preview renderer
- [docs/deployment.md](docs/deployment.md) — multi-device pattern + a real 2-fan install

## Repository layout

```
INSTALL.md                       # step-by-step setup guide
quietcool-lora32.yaml            # TTGO LoRa32 V2.1 / SX1278 — shared base config
quietcool-lora-v3.yaml           # Heltec/HiLetgo ESP32-S3 / SX1262 port
secrets.yaml.example             # copy to secrets.yaml (gitignored)
tests/                           # config regression tests (pytest/unittest)
tools/                           # display renderer + fan-frame generator
fonts/ images/                   # OLED assets (MDI webfont, fan bitmaps)
docs/                            # protocol, firmware analysis, hardware, display
```

## Safety

A whole-house fan moves a lot of air. Before energizing one: open enough windows
for makeup air, confirm combustion appliances can't backdraft, and keep a working
OEM control as a fallback. The checked-in templates preserve a strict causal
invariant: RF only ever originates from an explicit button press or Home
Assistant command, plus the bounded follow-ups those arm — the confirmation
query and the spaced re-fire attempts, both volatile and hard-limited. Nothing
transmits at boot, after OTA, on reconnect, from restored state, or from a
received frame, and a heard OEM-remote press cancels all pending automatic
work.


## Learn mode / porting to your own fan

Every QuietCool OEM sender ID is four bytes beginning with `CB`; the RF
profile and command format are universal. This firmware can therefore learn a
fan's ID from its OEM remote through the existing receive path.

### First-boot flow for another fan

1. Before compiling, change the top-level substitution in
   `quietcool-lora32.yaml` to:

   ```yaml
   substitutions:
     quietcool_sender_id: "0x00000000"
   ```

   This is deliberately a normal substitution, not a `!secret`, so the
   portable configuration has no extra secrets-file dependency. The checked-in
   default is `0x00000000` — the firmware ships in learn mode and captures
   your fan's ID on first boot.
2. Flash normally. When the persisted ID is zero, boot enters auto-learn and
   the OLED shows `LEARN / REMOTE X2`. Auto-learn stays armed - re-arming its
   120-second listening window as needed - for up to **15 minutes after
   power-on**, on the assumption that the installer is physically present at
   first power-on. Past that ceiling it disarms fully and the OLED returns to
   its normal (unprovisioned/OFF) layout; TX still refuses while unprovisioned
   regardless. See "Manual re-learn and forget" below to re-arm afterward.
3. Press a command on the OEM remote, wait more than 600 ms, then press the
   remote again within 60 seconds. Two separate button presses are the
   required workflow: only a real state-command frame (a speed/duration
   button press) can start or confirm a candidate, so the OEM's three 45 ms
   repeats within one press cannot confirm themselves, and the passive `66
   66` wake/status query can never complete a learn on its own - a
   requirement that also blocks a parked, unprovisioned unit from picking up
   a neighboring installation's ID from overheard query/command cross-talk.
4. On acceptance the OLED briefly shows `LEARNED / ID SAVED`, the
   `Remote Sender ID` Home Assistant text sensor publishes the captured
   four-byte ID (always beginning `CB`), and it is force-committed to NVS.
   Until an ID is set,
   `tx_burst` logs an error and refuses to transmit or increment `TX Count`.

Only a six-byte, `CB`-prefixed frame carrying a valid speed/duration
state-command (matching command bytes, a real speed nibble, a real duration
nibble) can become a candidate; the `66 66` query is rejected even from the
owner's own remote. A second valid frame must carry the same ID more than 600
ms but less than 60 seconds after the first. A different valid sender restarts
the two-frame count, which prevents a nearby neighbor's one-off remote press
from completing a candidate started by another sender. Learn frames are
consumed by the RX and storage path and never publish fan state or reach any
TX action.

### Manual re-learn and forget

- Press the Home Assistant `Learn Remote ID` button (in the device's
  Configuration section, disabled by default — enable the entity first), or
  hold the board's PRG
  button for 5-10 seconds. This opens a 120-second manual window and leaves the
  currently stored ID intact unless a new candidate is confirmed. The existing
  1-5 second PRG Off gesture ends at 4999 ms, so the gestures do not overlap.
  This is the required way to re-arm learning once the first-boot 15-minute
  window has elapsed.
- Press `Forget Remote ID` to write zero to NVS immediately, publish `unset`,
  and re-enter auto-learn (with its own fresh 15-minute ceiling, since Forget
  is itself a deliberate local/HA action) until a replacement remote is
  confirmed. Forget also durably suppresses the compiled default: even on a
  build compiled with a nonzero `quietcool_sender_id`, on_boot will **not**
  silently reseed that value on the next reboot, so a Forget stays forgotten
  across reboot and OTA. A later successful learn clears the suppression.
  Third-party builds should still keep the substitution at `0x00000000`.

`learned_sender_id` uses ESPHome's restored globals storage. A learned ID
survives ordinary reboot, OTA, and subsequent firmware updates that retain the
same global. A full flash/NVS erase removes it (along with the Forget
suppression flag); after such an erase, boot either applies the nonzero
compile-time seed or starts auto-learn when the seed is zero.

Acceptance requires two matching bursts from the same `CB`-prefixed sender more
than 600 ms apart — a **two-burst neighbor guard** so a single stray frame from a
neighbor's fan on the shared band can't provision your controller. While a learn
window is armed the OLED shows `LEARN / REMOTE X2`, then briefly `LEARNED / ID
SAVED` on success (previewed in `docs/display-previews/learn-active.png` and
`docs/display-previews/learn-confirmed.png`).


## Provenance & license

This is an independent reverse-engineering effort. The 433 MHz carrier and the
2-FSK nature were established from SDR captures; the exact register profile,
frame format, sender-ID mechanism, and command-byte structure were recovered by
dumping and disassembling the OEM remote's STM32 firmware (see
[docs/firmware-analysis.md](docs/firmware-analysis.md)). An early community
proof-of-concept ([ccrome/quiet-cool-rf-remote](https://github.com/ccrome/quiet-cool-rf-remote))
pointed at the general approach but was not used in the working implementation.

Code, tooling, and docs are MIT-licensed (see [LICENSE](LICENSE)). The OEM
firmware itself is not redistributed — only independently derived facts about the
protocol are documented here. "QuietCool" is a trademark of its owner; this
project is not affiliated with or endorsed by QuietCool.
