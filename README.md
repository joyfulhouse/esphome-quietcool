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

- **Direct RF fan control** — Off / Low / Medium / High and speed-aware
  1 / 2 / 4-hour timers, transmitted as the exact OEM frames.
- **Learn mode** — capture your fan's 4-byte sender ID by pressing its OEM remote
  twice. No packet sniffing or firmware extraction needed to onboard; the ID is
  persisted in NVS and survives reboots and OTA. See
  [Learn mode](#learn-mode--porting-to-your-own-fan).
- **Home Assistant native API** — a proper `fan` entity plus diagnostics
  (TX/RX counters, last command, learned sender ID, battery voltage/level).
- **Observed-state receive** — strictly validates and mirrors OEM-remote presses
  into the HA entity **without ever re-transmitting** (no RF echo).
- **On-device OLED** — animated fan icon, HH:MM:SS timer countdown, three
  HA-relayed temperatures (indoor / outdoor / attic) with semantic icons, and a
  WiFi / API / battery status row. Temperature sources are configurable from the
  HA UI, not hard-coded.
- **Safety-first** — never transmits at boot, after OTA, on API reconnect, from
  restored state, or from a received frame. Multi-model adversarially reviewed.
- **Multi-board & multi-fan** — one shared config, thin per-device wrappers.

## Supported hardware

| Board | Radio | MCU | Config | Status |
| --- | --- | --- | --- | --- |
| LilyGO TTGO LoRa32 **V2.1** (433 MHz) | SX1278 (SX127x) | ESP32 | `quietcool-lora32.yaml` | Verified on real fans |
| Heltec / HiLetgo ESP32 LoRa **V3** (433–510 MHz) | SX1262 (SX126x) | ESP32-S3 | `quietcool-lora-v3.yaml` | Port (see docs) |

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
fan via [Learn mode](#learn-mode--porting-to-your-own-fan).

## Documentation

- [docs/protocol.md](docs/protocol.md) — RF profile, frame format, command byte
- [docs/firmware-analysis.md](docs/firmware-analysis.md) — the reverse-engineering:
  memory map, register config, command-byte disassembly, per-unit ID mechanism
- [docs/hardware.md](docs/hardware.md) — boards, wiring, antenna, buying links
- [docs/display.md](docs/display.md) — OLED layout, icon language, preview renderer
- [docs/deployment.md](docs/deployment.md) — multi-device pattern + a real 2-fan install

## Repository layout

```
quietcool-lora32.yaml            # TTGO LoRa32 V2.1 / SX1278 — shared base config
quietcool-lora32-upstairs.yaml   # example second device (includes the base)
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
OEM control as a fallback. This firmware never transmits on its own — only from an
explicit button press or Home Assistant command.


## Learn mode / porting to your own fan

Every QuietCool OEM sender ID is four bytes beginning with `CB`; the RF
profile and command format are universal. This firmware can therefore learn a
fan's ID from its OEM remote through the existing receive path.

### First-boot flow for another fan

1. Before compiling, change the top-level substitution in
   `quietcool_lora32_ccrome.yaml` to:

   ```yaml
   substitutions:
     quietcool_sender_id: "0x00000000"
   ```

   This is deliberately a normal substitution, not a `!secret`, so the
   portable configuration has no extra secrets-file dependency. The checked-in
   default is `0xCB004739` for this installation.
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
   `Remote Sender ID` Home Assistant text sensor publishes a value such as
   `CB 00 47 39`, and the ID is force-committed to NVS. Until an ID is set,
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

- Press the Home Assistant `Learn Remote ID` button, or hold the board's PRG
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
