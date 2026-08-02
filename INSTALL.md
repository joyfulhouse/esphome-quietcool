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

## 1. Install from the ESPHome Device Builder (recommended)

You do **not** clone this repo to use it. The component is consumed like any
other ESPHome external component, from your existing ESPHome Device Builder
(the Home Assistant add-on or a standalone dashboard):

1. In the Builder, create a **new device** for your board (TTGO LoRa32 V2.1).
2. Replace the generated config with the shape of
   [`quietcool-cpp-example.yaml`](quietcool-cpp-example.yaml) — board, radio
   and entity blocks verbatim — but source the component from GitHub instead
   of a local path:

   ```yaml
   external_components:
     - source: github://joyfulhouse/esphome-quietcool@v0.2.0
       components: [quietcool]
   ```

   (Pin a release tag. `@main` tracks development.) Keep the example's
   `esp32:` block exactly: **esp-idf with `loop_task_stack_size: 16384` is
   REQUIRED** — codegen rejects anything less, because the Arduino default
   stack crash-looped a production controller. No `esphome: includes:` bridge
   is needed for the GitHub source; that bridge exists only for this repo's
   own local-path builds.
3. Add your Wi-Fi / API / OTA credentials the normal Builder way (its managed
   `secrets.yaml`), then **Install → Wirelessly / Plug into this computer**.
   Antenna on first. Later updates arrive like any other ESPHome device:
   the Builder shows *Update* and rebuilds from your config.

The Heltec/HiLetgo **V3 (SX1262)** board works the same way — use the SX1262
radio block shown at the bottom of the example config.

## 2. Developer install (from a checkout)

Only for working **on** the component itself — the repo's own configs build
from the local tree and carry the maintainers' deploy assumptions:

```bash
git clone https://github.com/joyfulhouse/esphome-quietcool.git
cd esphome-quietcool
uv venv .venv && uv pip install --python .venv/bin/python esphome
cp secrets.yaml.example secrets.yaml   # edit Wi-Fi + keys; gitignored
openssl rand -base64 32                # -> quietcool_lora32_api_key
.venv/bin/esphome run quietcool-cpp-lora32.yaml   # USB first; OTA later
```

The legacy YAML build for the V3 board lives under `legacy/`
(`.venv/bin/esphome run legacy/quietcool-lora-v3.yaml`, plus
`cp secrets.yaml legacy/secrets.yaml`); its pairing differs — see
[legacy/README.md](legacy/README.md). The rest of this guide describes the
C++ build, whichever way you installed it.

The firmware **never sends a fan command unless you ask it to** — a command
(anything that could change the fan) originates only from an explicit control
action in Home Assistant, plus the bounded confirmation query and spaced re-fire
attempts that action authorizes. It does, on its own, send a **non-energizing**
`66 66` **status query** in a few cases — most importantly, a provisioned unit
sends one shortly after boot (including the reboot after an OTA update) to
re-establish confirmed state. A status query cannot start the fan. So the radio
can be briefly live right after power-on: keep the antenna connected and observe
the normal whole-house-fan safety precautions while flashing.

## 3. Adopt in Home Assistant

Home Assistant should auto-discover the device (Settings → Devices & Services
→ ESPHome → *Configure*). Enter the API encryption key from your
`secrets.yaml` when asked.

> If discovery doesn't fire (common across VLANs), add the ESPHome integration
> manually with the device's IP address. On segmented networks, give the board
> a DHCP reservation so HA can always reach it.

## 4. Pair with your fan (learn mode)

The firmware ships unprovisioned (`quietcool_sender_id: "0"`) — no sender ID is
hard-coded. Unlike the legacy build, the C++ build does **not** auto-open a learn
window at boot and shows **no OLED learn prompt**; you start learning explicitly
and watch progress in Home Assistant.

1. In HA, open the device, enable the **`Learn Remote ID`** button (Configuration
   section, disabled by default), and press it to open the learn window.
2. Stand near the controller and **operate the fan you are pairing with its own
   OEM remote — press a real speed/duration button about three times, roughly a
   second apart**, all within 60 seconds. Do this immediately and keep at it
   for the whole window: Learn binds whichever fan it hears, so the target fan
   must be transmitting while the window is open.
3. Watch the device **logs**
   (`.venv/bin/esphome logs quietcool-cpp-lora32.yaml`) and the
   `Command Confirmation Status` sensor. On success the fan's four-byte ID
   (always beginning `CB`) is bound and persisted to flash; it survives reboots
   and OTA.

**Why three presses, and why operate the fan.** The C++ build binds only after
**three independent sightings** of the same remote — each at least 600 ms after
the last one it counted — so a single burst of frames counts once and **two
presses is not enough**. Operating the fan itself is required, not just
helpful: the fan's own ~1.2 s status self-reports count as extra sightings,
and — more importantly — the safety guard depends on it. If a *different* `CB`
fan is heard during the window (a neighbor's, or your other unit), the
controller **aborts and keeps whatever ID was already bound** rather than risk
binding the wrong fan. But that guard can only fire when your fan is actually
heard: if the target fan stays silent for the whole window and a lone foreign
fan is the only sender heard, there is no second observation to trip the
abort, and the controller **will bind the foreign fan**. A unit that is
already paired never learns on its own, so the exposed case is exactly this
first pairing — an unprovisioned unit within RF range of another active
QuietCool — which is why step 2 is part of the procedure and not a
convenience.

**Re-pairing is deliberately two steps: Forget, then Learn.** Once an ID is
bound (learned or compiled in), pressing `Learn Remote ID` alone is **refused**
— the `Command Confirmation Status` sensor shows `refused` and the device log
shows `reason=already_provisioned` — and the existing binding is untouched.
Press `Forget Remote ID` first to erase the stored ID, then `Learn Remote ID`
to open a fresh window. This exists because a single accidental Learn press on
a provisioned unit could previously open a window in which its binding could be
replaced by whichever lone fan happened to transmit. Full details are in the
[README's learn-mode section](README.md#learn-mode--porting-to-your-own-fan).

## 5. Try it

- In HA, turn **QuietCool Fan** on and pick a speed. Until the fan has proved
  how many speeds it has, the entity lists three but **Medium is unreachable** —
  a speed press can only transmit Low or High, because Medium *stops* a
  two-speed fan. The first confirmed report settles it: a two-speed fan narrows
  the listed speeds to two, and a three-speed fan unlocks Medium. The confirmed
  capability is written to flash immediately, so every later boot lists the
  fan's real speeds from the first Home Assistant connection instead of
  re-learning them. Within one session the listed count only ever narrows;
  widening it again takes a reboot, because Home Assistant caches the count
  when it connects. The fan responds like the OEM remote pressed the button
  (same frames, same 3× burst). The request does not optimistically change the
  fan entity; wait for `Fan State Known` plus the requested entity state before
  treating it as physical success.
- Press a button on the **OEM remote**: the controller records it in RF
  diagnostics, cancels conflicting local work, and never echoes it over RF.
  It deliberately does not update the safety-facing fan entity from the
  command alone, because hearing the command does not prove that the fan
  accepted it. Press **Refresh Fan State** to request query-consensus evidence.
  A downstream HA automation is a separate explicit command source.
- **Timers.** The **Fan Timer** select runs the fan for 1 / 2 / 4 / 8 / 12
  hours, or `None` for no timer at all.

  **Every option transmits, and every option runs the fan.** Picking a duration
  while the fan is stopped **starts it**, at Low. `None` does not mean "do
  nothing" — it means "no timer, run until stopped", so on a fan whose timer has
  already expired and which has therefore stopped, `None` **restarts it**. The
  protocol has no non-actuating way to clear a timer, which is why the harmless-
  looking option is not harmless. Open a window before running the fan.

  The select shows only what the fan has **confirmed**: picking an option
  transmits and then waits, and the shown value moves when evidence arrives, not
  when you click. A refused command leaves it unchanged.

  Note the two senses of `None`: as a **display** it means "no timer
  programmed" — true for a continuously running fan and for a stopped one
  (whether the fan is running is the fan entity's job to show). **Selecting**
  it is what transmits run-continuously.

  There is no "off" in this list — stopping the fan belongs to the fan entity.
  On the wire, duration `0x0` stops the fan and `0xF` runs it continuously; both
  mean "no countdown", which is why they look redundant, but they are opposites.

  The read-only `Timer Remaining` sensor and the OLED countdown reflect a timer
  only after this controller's own exchange is query-confirmed (gated by
  `Timer State Known`); a passively heard report showing an active timer has
  unknown age and cannot authorize a countdown.

## 6. Optional: temperatures on the OLED

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

## 7. Optional: a second fan

Create a small wrapper YAML that overrides the identity substitutions and
includes the base config as a package, add its secrets, flash, and let it
learn its own remote. The pattern is in
[docs/deployment.md](docs/deployment.md).

## Entity reference

These are the entities on the **C++ build** (`quietcool-cpp-lora32.yaml`). The
diagnostics marked *disabled by default* exist but are hidden until you enable
them in Home Assistant.

| Entity | Type | Purpose |
| --- | --- | --- |
| `QuietCool Fan` | fan | Off / Low / High, plus Medium once the fan confirms three speeds |
| `Fan Timer` | select | None / 1 / 2 / 4 / 8 / 12 hours. **Every option runs the fan** — a duration starts a stopped fan at Low, and `None` (no timer, run continuously) restarts a fan whose timer expired |
| `Timer Remaining` | sensor | Confirmed countdown in minutes (also on the OLED) |
| `Fan State Known` | binary sensor (diagnostic) | On only when the fan entity is backed by correlated physical evidence |
| `Fan Confirmed Off` | binary sensor (diagnostic) | Atomic Off assertion; false combines running and unknown |
| `Timer State Known` | binary sensor (diagnostic) | On only when timer-program metadata is backed by correlated physical evidence |
| `Fan Timer Remaining Known` | binary sensor (diagnostic) | On only when the remaining-time countdown is correlated evidence |
| `Controller Fault` | binary sensor (diagnostic, `device_class: problem`) | **On = the controller has degraded and is terminal until reboot.** Control and observation have stopped; the `*_Known` flags drop and `Command Confirmation Status` reads `unavailable`. Power-cycle or reboot the device to clear it. |
| `Command Confirmation Status` | text sensor | Pending, confirmed, mismatch, refused, or bounded failure |
| `Fan Evidence Source` | text sensor | Which exchange last produced authoritative state (boot / manual / recovery query, post-command consensus, …) |
| `Refresh Fan State` | button | Non-energizing status query using response consensus; active timers remain unknown-age |
| `Learn Remote ID` | button (config, disabled by default) | Open a learn window — refused while an ID is bound (Forget first, see §5) |
| `Forget Remote ID` | button (config, disabled by default) | Erase the stored ID and return to learn mode |
| `Battery Voltage` / `Battery Level` | sensors | On-board LiPo monitoring |
| `WiFi Signal`, `Uptime`, `IP Address`, `Restart` | misc | Housekeeping |
| `Last TX Command` | text sensor (diagnostic, disabled by default) | The last command byte this controller transmitted, e.g. `0xB1` |
| `Last Valid RX Frame` | text sensor (diagnostic, disabled by default) | The last frame that passed validation |
| `Last Confirmed Fan State` | text sensor (diagnostic, disabled by default) | The confirmed state, e.g. `high 1h`; `unknown` when nothing is confirmed |
| `Fan Speed Capability` | text sensor (diagnostic, disabled by default) | `1`, `2`, `3`, or `unknown` — never a fabricated value |
| `Remote Sender ID` | text sensor (diagnostic, disabled by default) | The bound fan's ID, e.g. `0xCB004739` |
| `TX Count` | sensor (diagnostic, disabled by default) | Transmissions since boot. A **flat** count while the OEM remote retries is what proves this bridge is not jamming it |
| `RX Valid Count` / `RX Rejected Count` | sensors (diagnostic, disabled by default) | Frames that passed / failed validation since boot |

The `Learn Remote ID` and `Forget Remote ID` buttons live in the device's
**Configuration** section and ship **disabled by default**. On the C++ build
pairing is a deliberate action, so enable `Learn Remote ID` and press it to
start (see §5): device page → the entity → gear icon → *Enabled*.

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
| `Command Confirmation Status` | Pending, confirmed, mismatch, refused, or bounded failure |
| `Fan Evidence Source` | Which exchange last produced authoritative state |
| `Fan State Known` | Whether the fan entity currently represents correlated physical state |
| `Fan Confirmed Off` | Atomic true-only-for-authoritatively-confirmed-Off signal |
| `Timer State Known` | Whether timer-program metadata currently represents correlated physical state |

The fan entity holds the last query-confirmed state, and `Fan Evidence Source`
records how it was obtained. If your controller is
mounted far from the fan, confirmation may intermittently time out (the fan's
replies are much weaker than its reception). The bounded command/re-fire
schedule still runs, but without confirmation the physical outcome is unknown;
you'll see `no response consensus` in `Command Confirmation Status` instead of
`confirmed`. A `Refresh Fan State` button sends a non-energizing status query
on demand and applies the same response-consensus rules as the command loop.
An active-timer reply reports its programmed duration but not its age, so a
manual query keeps the timer and safety fan entity unknown rather than
synthesizing an authoritative countdown. A provisioned unit already sends one
non-energizing status query shortly after boot (and after the reboot an OTA
triggers) to re-establish confirmed state; an API reconnect on its own does not
transmit. Use `Refresh Fan State` if you want an additional resync on demand.

The custom control path is confirmation-driven: an HA request does not publish
its requested On/Off state. There is nevertheless one unavoidable ESPHome API
boundary: the native Fan API has no “unknown” representation and sends its raw
default Off/Low fields when HA first subscribes. Therefore **never use the fan
entity alone as a safety confirmation**. For an Off interlock, use the atomic
diagnostic instead of joining two separately delivered entity updates:

```jinja2
{{ is_state('binary_sensor.YOUR_DEVICE_fan_confirmed_off', 'on') }}
```

Use the corresponding `Timer State Known` diagnostic before trusting the
`Timer Remaining` countdown. The C++ build presents no guessed timer state at
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

- **Fan doesn't react to HA commands** — check the antenna, then confirm the
  controller is paired: if it never learned a remote, TX deliberately refuses
  (the device log shows the refusal and `Fan State Known` stays off). Distance
  matters less than you'd think (+17 dBm reaches across a house), but metal
  ducting between controller and fan receiver doesn't help. Sending a command
  only proves a burst went out; check `Command Confirmation Status` for whether
  the fan actually confirmed it.
- **Learn never confirms** — the C++ build needs **three** independent sightings
  of the same remote, each ≥ ~0.6 s apart and all within 60 s, and each must be
  a real speed/off button (not a bare status ping). **Two presses is not
  enough.** Make sure you pressed `Learn Remote ID` first (it ships disabled
  under Configuration — enable it), and that no *other* `CB` fan is transmitting
  nearby — a second sender aborts the
  window. Operating the target fan itself helps: its ~1.2 s self-reports count
  toward the three.
- **Learn is refused immediately** — `Command Confirmation Status` flips to
  `refused` (log: `reason=already_provisioned`) the moment you press `Learn
  Remote ID`. The controller already has a bound ID (learned earlier, or
  compiled in via `quietcool_sender_id`). Re-pairing is deliberately two steps:
  press `Forget Remote ID`, then `Learn Remote ID` (see §5).
- **HA entity doesn't follow the OEM remote** — this is intentional: a heard
  command is diagnostics-only, not proof of fan actuation. Use **Refresh Fan
  State** to seek authoritative consensus. If nothing changes even after a
  Refresh, the remote may be unlearned or out of RX range (the device log shows
  received frames).
- **Blank OLED on the V3 board** — the V3's display is powered through Vext;
  the config drives it, but early clones vary. See the `PIN CONFIDENCE` notes
  in `legacy/quietcool-lora-v3.yaml` and [docs/hardware.md](docs/hardware.md).
- **HA can't connect after adoption** — almost always network segmentation
  (mDNS across VLANs). Use the device's IP directly and reserve it in DHCP.

Still stuck? Open a [GitHub issue](https://github.com/joyfulhouse/esphome-quietcool/issues)
with the device log (`.venv/bin/esphome logs quietcool-cpp-lora32.yaml`).
