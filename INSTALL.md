# Installation guide

End-to-end setup: flash the board, adopt it in Home Assistant, and pair it with
your fan using nothing but its OEM remote. Total hands-on time is about 20
minutes, most of it waiting for the first compile.

## What you need

**Hardware**

- A supported board (see [docs/hardware.md](docs/hardware.md)):
  - **LilyGO TTGO LoRa32 V2.1, 433 MHz** (SX1278) — the verified reference
    board: <https://amzn.to/4vBvqOU>
  - **HiLetgo / Heltec ESP32 LoRa V3** (SX1262): <https://amzn.to/4wagWqi> —
    builds, awaiting hardware verification
- A **433 MHz antenna**, attached **before** the board ever transmits (keying
  the PA into an open antenna port can damage it — both linked kits include one)
- A USB data cable
- Your fan's working **OEM RF remote** (used once, to teach the controller)

**Software**

- Python 3.11+ with [`uv`](https://docs.astral.sh/uv/) (or any way to install
  ESPHome ≥ 2025.11)
- Home Assistant with the ESPHome integration

## 1. Get the code and install ESPHome

```bash
git clone https://github.com/joyfulhouse/esphome-quietcool.git
cd esphome-quietcool
uv venv .venv && uv pip install --python .venv/bin/python esphome
```

## 2. Create your secrets

```bash
cp secrets.yaml.example secrets.yaml
```

Edit `secrets.yaml` with your Wi-Fi credentials, and replace the placeholder
API key with a real one:

```bash
openssl rand -base64 32   # paste the output as quietcool_lora32_api_key
```

`secrets.yaml` is gitignored; nothing sensitive lives in the config itself.

## 3. Flash over USB

Antenna on first. Then:

```bash
.venv/bin/esphome run quietcool-lora32.yaml     # TTGO LoRa32 V2.1
# or
.venv/bin/esphome run quietcool-lora-v3.yaml    # Heltec/HiLetgo V3
```

Pick your serial port when prompted. The first compile takes a few minutes;
every later update can go over the air (same command, choose OTA).

The firmware **never transmits on its own** — not at boot, not after OTA, not
on reconnect — so flashing is safe to do with the fan installed.

## 4. Adopt in Home Assistant

Home Assistant should auto-discover the device (Settings → Devices & Services
→ ESPHome → *Configure*). Enter the API encryption key from your
`secrets.yaml` when asked.

> If discovery doesn't fire (common across VLANs), add the ESPHome integration
> manually with the device's IP address. On segmented networks, give the board
> a DHCP reservation so HA can always reach it.

## 5. Pair with your fan (learn mode)

The firmware ships unprovisioned — no sender ID is hard-coded — and boots
straight into learn mode: the OLED shows **`LEARN / REMOTE X2`**.

1. Stand near the controller with the fan's OEM remote.
2. Press any speed button (e.g. **Low**) on the remote.
3. Wait at least one second, then press a button again (e.g. **Off**), within
   60 seconds of the first press.
4. The OLED flashes **`LEARNED / ID SAVED`** and the `Remote Sender ID`
   sensor in HA shows your fan's four-byte ID (always beginning `CB`).

That's it — the ID is persisted to flash and survives reboots and OTA updates.
Two *separate* presses are required by design (a two-burst neighbor guard), so
a stray frame from a neighbor's installation can never pair itself. Full
details, including re-arming learn later and `Forget Remote ID`, are in the
[README's learn-mode section](README.md#learn-mode--porting-to-your-own-fan).

## 6. Try it

- In HA, turn **QuietCool Fan** on and pick a speed that your fan actually
  supports — the public template exposes Low / Medium / High, but it does not
  yet query the receiver's speed capability or hide Medium on a two-speed fan.
  The fan
  responds like the OEM remote pressed the button (same frames, same 3×
  burst).
- Press a button on the **OEM remote**: the HA entity updates to match within
  a second. Control is fully **bi-directional** — the controller listens to
  the same RF channel and mirrors remote presses into HA without ever
  re-transmitting them (no RF echo, no loop).
- The **Fan Timer** select arms the OEM countdown at the fan's current speed —
  pick 1 / 2 / 4 / 8 / 12 hours, or `None` to cancel a running timer (the fan
  keeps running at its current speed). The OLED shows the remaining time as
  `HH:MM:SS`, and the select stays in sync no matter who started the timer:
  start one from the physical OEM remote and the select snaps to the matching
  duration within a second.

## 7. Optional: temperatures on the OLED

The display's indoor / outdoor / attic readouts come from three Home Assistant
entities, configurable **without reflashing**. Create three template sensor
helpers (Settings → Devices & Services → Helpers → *Create helper* →
*Template* → *Template a sensor*) with these entity IDs, each returning your
preferred source's temperature:

| Helper entity ID | Shown as |
| --- | --- |
| `sensor.quietcool_display_indoor` | Indoor (house icon) |
| `sensor.quietcool_display_outdoor` | Outdoor (sun icon) |
| `sensor.quietcool_display_attic` | Attic (roof icon) |

Re-point a source any time by editing the helper's template. Missing or
unavailable sources render as `--`; they can never affect the fan. (You can
also point the `display_*_entity` substitutions directly at existing sensors
at compile time — see [docs/deployment.md](docs/deployment.md).)

## 8. Optional: a second fan

Create a small wrapper YAML that overrides the identity substitutions and
includes the base config as a package, add its secrets, flash, and let it
learn its own remote. The pattern is in
[docs/deployment.md](docs/deployment.md).

## Entity reference

| Entity | Type | Purpose |
| --- | --- | --- |
| `QuietCool Fan` | fan | Off / Low / Medium / High — the only fan control |
| `Fan Timer` | select | OEM timer at the current speed: None / 1 / 2 / 4 / 8 / 12 h, synced with remote-started timers |
| `Timer Remaining` | sensor | Countdown in seconds (also on the OLED) |
| `Refresh Fan State` | button | Non-energizing status query; resyncs entity/timer/select from the fan |
| `Learn Remote ID` | button (config, disabled by default) | Re-arm a 120 s learn window |
| `Forget Remote ID` | button (config, disabled by default) | Erase the stored ID and re-enter learn mode |
| `Remote Sender ID` | text sensor | The learned four-byte ID (`CB …`) |
| `TX Count`, `RX Valid Count`, `RX Rejected Count` | sensors | RF diagnostics |
| `Last TX Command`, `Last Valid RX Frame` | text sensors | RF debugging |
| `Battery Voltage` / `Battery Level` | sensors | On-board LiPo monitoring |
| `WiFi Signal`, `Uptime`, `IP Address`, `Restart`, `Status LED` | misc | Housekeeping (`Status LED` under Configuration) |

The `Learn Remote ID` and `Forget Remote ID` buttons live in the device's
**Configuration** section and ship **disabled by default** (pairing normally
happens through the automatic first-boot learn window, so they're one-time
tools). To use one, enable it first: device page → the entity → gear icon →
*Enabled*.

### Closed-loop confirmation entities

The OEM protocol supports a real query/response exchange, and the templates
implement it: after each explicit command the controller sends the OEM `66 66`
status query, listens for the fan's fixed six-byte reply, compares confirmed
and requested state, and allows only bounded, spaced continuation attempts if
the fan hasn't confirmed. Three extra diagnostics expose the result:

| Entity | Meaning |
| --- | --- |
| `Last Confirmed Fan State` | Last state reported by the fan itself |
| `Command Confirmation Status` | Pending, confirmed, mismatch, or bounded failure |
| `Fan Speed Capability` | Capability metadata reported by the receiver (e.g. `2-speed`) |

`Last TX Command` still records only that a command was attempted;
`Last Confirmed Fan State` is the fan's own answer. If your controller is
mounted far from the fan, confirmation may intermittently time out (the fan's
replies are much weaker than its reception); commands still go through and the
spaced re-fire backstop still runs — you'll just see `no response consensus`
in `Command Confirmation Status` instead of `confirmed`. A `Refresh Fan
State` button sends a single non-energizing status query on demand to resync
the entity, timer, and Fan Timer select from the fan's reply (the `Query Fan
State (probe)` diagnostic does the same plus 15 s of raw RX logging), and the
controller automatically queries once ~12 s after every boot/OTA so a reboot
can't leave HA guessing.

## Troubleshooting

- **Fan doesn't react to HA commands** — check the antenna, then check
  `Remote Sender ID`: if it reads `unset`, learn mode hasn't completed and TX
  deliberately refuses (watch `TX Count` — it won't increment). Distance
  matters less than you'd think (+17 dBm reaches across a house), but metal
  ducting between controller and fan receiver doesn't help. An incremented
  `TX Count` proves only that a burst was sent; check
  `Command Confirmation Status` for whether the fan actually confirmed it.
- **Learn never confirms** — the two presses must be more than ~0.6 s and less
  than 60 s apart, and each must be a real speed/off/timer button. If the
  first-boot window (15 minutes) has lapsed, press `Learn Remote ID` in HA
  (under Configuration; enable the entity first — it ships disabled) or hold
  the board's PRG button for 5–10 s to re-arm.
- **HA entity doesn't follow the OEM remote** — watch `RX Valid Count` while
  pressing the remote. If it doesn't increment, the remote hasn't been learned
  (or you're out of RX range); if it increments but the entity doesn't move,
  check `Last Valid RX Frame` and open an issue with its contents.
- **Blank OLED on the V3 board** — the V3's display is powered through Vext;
  the config drives it, but early clones vary. See the `PIN CONFIDENCE` notes
  in `quietcool-lora-v3.yaml` and [docs/hardware.md](docs/hardware.md).
- **HA can't connect after adoption** — almost always network segmentation
  (mDNS across VLANs). Use the device's IP directly and reserve it in DHCP.

Still stuck? Open a [GitHub issue](https://github.com/joyfulhouse/esphome-quietcool/issues)
with the device log (`.venv/bin/esphome logs quietcool-lora32.yaml`).
