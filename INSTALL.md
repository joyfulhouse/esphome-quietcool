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
    config/compile validated with ESPHome 2026.7.0, awaiting hardware
    verification
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

The firmware **never transmits merely because it booted** — not after OTA and
not on reconnect. It transmits only after an explicit control/Refresh action
and for the bounded query/re-fire work that action authorizes. Keep the antenna
connected and observe the normal whole-house-fan safety precautions while
flashing.

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
  supports. The entity statically exposes Low / Medium / High, while the
  `Fan Speed Capability` diagnostic learns what the receiver reports after a
  confirmed exchange; it cannot dynamically hide Medium on a two-speed fan.
  The fan responds like the OEM remote pressed the button (same frames, same
  3× burst). The request does not optimistically change the fan entity; wait
  for `Fan State Known` plus the requested entity state before treating it as
  physical success.
- Press a button on the **OEM remote**: the controller records it in RF
  diagnostics, cancels conflicting local work, and never echoes it over RF.
  It deliberately does not update the safety-facing fan entity from the
  command alone, because hearing the command does not prove that the fan
  accepted it. Press **Refresh Fan State** to request query-consensus evidence.
  A downstream HA automation is a separate explicit command source.
- The **Fan Timer** select arms the OEM countdown at the fan's current speed —
  pick 1 / 2 / 4 / 8 / 12 hours. `None` is a safe no-op because the protocol
  has no proven non-actuating “clear timer” command; explicitly select a fan
  speed to send continuous mode instead. The select and OLED countdown update
  only after this controller's own timer command is query-confirmed. A passive
  command or a manual Refresh report showing an active timer has unknown age
  and cannot authorize a countdown.

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
| `Fan Timer` | select | OEM timer at the current speed: 1 / 2 / 4 / 8 / 12 h; `None` sends no RF; published only while timer state is known |
| `Timer Remaining` | sensor | Confirmed countdown in minutes (also rendered as `HH:MM:SS` on the OLED) |
| `Fan State Known` | binary sensor (diagnostic) | On only when the fan entity is backed by correlated physical evidence |
| `Fan Confirmed Off` | binary sensor (diagnostic) | Atomic Off assertion; false combines running and unknown |
| `Timer State Known` | binary sensor (diagnostic) | On only when timer metadata/countdown is backed by correlated physical evidence |
| `Refresh Fan State` | button | Non-energizing status query using response consensus; active timers remain unknown-age |
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
implement it: a new explicit command starts a bounded transaction, the
controller sends the OEM `66 66` status query, listens for the fan's fixed
six-byte reply, and compares confirmed and requested state. If the fan has not
confirmed, the existing spaced continuation schedule remains eligible.
Equivalent Off calls made while an Off transaction is active join that
transaction without another transmission or attempt-counter reset. The key
diagnostics expose confirmation and state knowledge:

| Entity | Meaning |
| --- | --- |
| `Last Confirmed Fan State` | Last state reported by the fan itself |
| `Command Confirmation Status` | Pending, confirmed, mismatch, or bounded failure |
| `Fan Speed Capability` | Capability metadata reported by the receiver (e.g. `2-speed`) |
| `Fan State Known` | Whether the fan entity currently represents correlated physical state |
| `Fan Confirmed Off` | Atomic true-only-for-authoritatively-confirmed-Off signal |
| `Timer State Known` | Whether timer metadata, select, and countdown currently represent correlated physical state |

`Last TX Command` still records only that a command was attempted;
`Last Confirmed Fan State` is the fan's own answer. If your controller is
mounted far from the fan, confirmation may intermittently time out (the fan's
replies are much weaker than its reception). The bounded command/re-fire
schedule still runs, but without confirmation the physical outcome is unknown;
you'll see `no response consensus` in `Command Confirmation Status` instead of
`confirmed`. A `Refresh Fan State` button sends a non-energizing status query
on demand and applies the same response-consensus rules as the command loop
(the `Query Fan State (probe)` diagnostic also opens 15 s of raw RX logging).
An active-timer reply reports its programmed duration but not its age, so a
manual query keeps the timer and safety fan entity unknown rather than
synthesizing an authoritative countdown. Boot,
OTA, and API reconnect never query or otherwise transmit; use Refresh when an
explicit post-restart resync is wanted.

The custom control path is confirmation-driven: an HA request does not publish
its requested On/Off state. There is nevertheless one unavoidable ESPHome API
boundary: the native Fan API has no “unknown” representation and sends its raw
default Off/Low fields when HA first subscribes. Therefore **never use the fan
entity alone as a safety confirmation**. For an Off interlock, use the atomic
diagnostic instead of joining two separately delivered entity updates:

```jinja2
{{ is_state('binary_sensor.YOUR_DEVICE_fan_confirmed_off', 'on') }}
```

Use the corresponding `Timer State Known` diagnostic before acting on the timer
select or countdown. The timer select is not initialized to a guessed `None` at
boot. A new command and every actual non-query burst (including each spaced
re-fire) invalidate both known flags. Outgoing state/timer requests do not
optimistically arm or clear confirmed timer metadata; a failed or unanswered
transaction leaves the timer unknown. A trusted countdown is armed only when a
locally initiated timer command is confirmed and anchored to that command's
completion time. Its estimated expiry invalidates fan/timer authority and
publishes no guessed Off because the fan sends no RF expiry event.

`Fan Confirmed Off` proves the last authoritative query consensus was Off and
is forced false on every detected invalidation. It is not a continuous motor
sensor: if every frame from a later OEM press is missed, RF state can become
stale. Use an explicit Refresh or independent airflow/motor sensing where a
fresh physical assertion is required.

This matters because on 2026-07-19 optimistic Off publication and interlock
re-entry produced 107 fan-state transitions in 73.34 seconds, 54 interlock runs,
and 118 RF bursts (354 application frames). All 53 mismatches still showed five
Off attempts remaining because each re-entry reset the transaction. The
confirmation-driven control path plus equivalent-active-request coalescing
closes both firmware paths while preserving the spaced re-fire schedule. Stale
queue entries are rejected before airtime, physical OEM queries reserve a
two-second holdoff, and consecutive query windows cannot count the preceding
response tail as fresh evidence. On 2026-07-19 the corrected logic was flashed
once to a downstream SX1278 controller. After 62 idle seconds with zero TX, a
manual Refresh confirmed Off; three rapid HA Off calls then joined one
transaction that used one Off burst plus one query and confirmed `90` after one
command and one query. HA recorded no fan or interlock state transitions during
the test. This validates the corrected RF/entity behavior on SX1278 but is not
an independent motor measurement. The V3 build still awaits hardware testing.

`tx_burst` is a bounded queue (`max_runs: 5`). ESPHome may reject an execution
beyond that capacity, so the firmware does not promise that every rapid press
becomes its own on-air burst. A new transaction and its re-fire command are
armed before enqueue; automatic confirmation/re-fire waits for the active burst
to finish, clears obsolete queued work, and executes the latest desired action.

## Troubleshooting

- **Fan doesn't react to HA commands** — check the antenna, then check
  `Remote Sender ID`: if it reads `unset`, learn mode hasn't completed and TX
  deliberately refuses (watch `TX Count` — it won't increment). Distance
  matters less than you'd think (+17 dBm reaches across a house), but metal
  ducting between controller and fan receiver doesn't help. An incremented
  `TX Count` proves only that a three-frame burst was sent; check
  `Command Confirmation Status` for whether the fan actually confirmed it.
- **Learn never confirms** — the two presses must be more than ~0.6 s and less
  than 60 s apart, and each must be a real speed/off/timer button. If the
  first-boot window (15 minutes) has lapsed, press `Learn Remote ID` in HA
  (under Configuration; enable the entity first — it ships disabled) or hold
  the board's PRG button for 5–10 s to re-arm.
- **HA entity doesn't follow the OEM remote** — this is intentional: a heard
  command is diagnostics-only, not proof of fan actuation. `RX Valid Count` and
  `Last Valid RX Frame` should still update, and **Refresh Fan State** can seek
  authoritative consensus. If the counter does not increment, the remote may
  be unlearned or out of RX range.
- **Blank OLED on the V3 board** — the V3's display is powered through Vext;
  the config drives it, but early clones vary. See the `PIN CONFIDENCE` notes
  in `quietcool-lora-v3.yaml` and [docs/hardware.md](docs/hardware.md).
- **HA can't connect after adoption** — almost always network segmentation
  (mDNS across VLANs). Use the device's IP directly and reserve it in DHCP.

Still stuck? Open a [GitHub issue](https://github.com/joyfulhouse/esphome-quietcool/issues)
with the device log (`.venv/bin/esphome logs quietcool-lora32.yaml`).
