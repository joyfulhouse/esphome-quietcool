# HiLetgo / Heltec ESP32 LoRa V3 (SX1262): hardware bring-up guide

This guide tests the V3 port on real hardware and collects evidence the
maintainers can act on. It works through every "not yet verified" item in
[hardware.md](hardware.md) and then the full closed-loop fan behaviour that is
already verified on the SX1278 reference board.

You record results in [v3-bringup-report.md](v3-bringup-report.md) as you go,
then attach that file and the saved logs to a GitHub issue.

**Firmware:** [`quietcool-cpp-lora-v3-bringup.yaml`](../quietcool-cpp-lora-v3-bringup.yaml)
is the C++ build with the same radio block as
`quietcool-cpp-example-sx126x.yaml`, plus Wi-Fi/API/OTA, all diagnostics turned
on, an RX log tap (bytes, RSSI, SNR), and one test entity for each
uncertain pin. Test entities are named `Test: …`.

---

## 0. Before you start

You need:

- The V3 board, with the **433 MHz antenna screwed on before it is ever
  powered**. Powering the radio with no antenna can damage the PA.
- A USB **data** cable. Most V3 units have a CP210x USB-serial chip, so
  Windows and macOS may need the Silicon Labs CP210x driver.
- The QuietCool fan and its working **OEM remote**.
- An open window or door whenever the fan may run. A whole-house fan can
  pull exhaust back down a flue.
- Optional: a multimeter and a 1S LiPo with the board's JST connector, for the
  battery test.
- A computer with Python 3.11+.

Set up the checkout once:

```bash
git clone https://github.com/joyfulhouse/esphome-quietcool.git
```

```bash
cd esphome-quietcool
```

```bash
python3 -m venv .venv && .venv/bin/pip install esphome
```

```bash
cp secrets.yaml.example secrets.yaml
```

Edit `secrets.yaml`: set `wifi_ssid` and `wifi_password`, then replace the
three `quietcool_lora_v3_*` values. Generate the API key with:

```bash
openssl rand -base64 32
```

Make a folder for evidence. Every log in this guide is saved there:

```bash
mkdir -p bringup-logs
```

Record `.venv/bin/esphome version` in the report.

---

## 1. Identify the board

1. Take a clear photo of **both sides** of the board. Make sure the silkscreen
   version (`V3`, `V3.1`, `V3.2`…) and the radio module markings are readable.
   Some V3.2 units add an RF front-end module, so the version matters.
2. Plug in USB (antenna already on) and read the chip details:

   ```bash
   .venv/bin/python -m esptool flash-id 2>&1 | tee bringup-logs/00-esptool.log
   ```

   Expect `ESP32-S3` and an 8 MB flash. If esptool cannot connect, hold
   **PRG**, tap **RST**, release **PRG** and run it again.

---

## 2. Build and flash over USB

```bash
.venv/bin/esphome run quietcool-cpp-lora-v3-bringup.yaml 2>&1 | tee bringup-logs/01-first-boot.log
```

Choose the USB serial port when asked. Leave it running for **about 60
seconds** after boot so the full config dump is captured, then press Ctrl-C.

**Pass:**

- Build finishes with `Successfully compiled program`.
- The board boots and joins Wi-Fi (`WiFi Connected`, IP address shown).
- The `SX126x:` config block appears with CS 8, BUSY 13, RST 12, DIO1 14 and
  frequency 433.92 MHz.
- **No** `Unsupported chip version`, `Wait busy timeout` or
  `component … marked as failed`. The first means the SPI pins or radio power
  are wrong. The second means the BUSY pin or the TCXO setting is wrong.
- **No** `QuietCool controller degraded`.

Then adopt the device in Home Assistant (Settings → Devices & Services →
ESPHome) with your V3 API key. From now on, stream logs over Wi-Fi with:

```bash
.venv/bin/esphome logs quietcool-cpp-lora-v3-bringup.yaml --device quietcool-lora-v3.local
```

Use the IP address instead if mDNS doesn't resolve.

---

## 3. Board peripherals (the PIN CONFIDENCE items)

None of these tests transmit.

| # | Test | How | Expected |
| --- | --- | --- | --- |
| 3a | I2C / OLED found | Search `01-first-boot.log` for the I2C scan | `Found device at address 0x3C` |
| 3b | OLED + Vext | Look at the screen | A white outline with a filled square that jumps left/right every second |
| 3c | Status LED polarity | Toggle `Test: Onboard LED (GPIO35, non-inverted)` in HA | Write down whether **ON lights the LED** (active-high) or **OFF lights it** (active-low) |
| 3d | PRG button | Hold the **PRG** button | `Test: PRG Button (GPIO0)` reads On while held, Off when released |
| 3e | VBAT ADC, no battery | No LiPo attached; read `Test: VBAT ADC Raw (GPIO1)` | Record the value (likely near 0 V, or noise) |
| 3f | VBAT ADC, battery | Attach the LiPo. Measure the cell with a multimeter. Read raw and `Battery Voltage (x4.9)` | `Battery Voltage (x4.9)` within ~0.1 V of the multimeter. Raw ≈ cell ÷ 4.9 |
| 3g | VBAT enable polarity | With the battery attached, turn `Test: VBAT ADC Enable (GPIO37)` off, wait 20 s, then on again | Raw drops to ~0 V when off and recovers when on. If it's the other way round, the enable pin is active-high |

Take a photo of the OLED for 3b. If the screen stays blank, the log will show
whether the I2C scan found nothing (Vext/power) or the display failed to set
up (reset pin).

---

## 4. Passive receive (no transmit)

This shows the SX1262 can hear the fan protocol. Start a log capture:

```bash
.venv/bin/esphome logs quietcool-cpp-lora-v3-bringup.yaml --device quietcool-lora-v3.local 2>&1 | tee bringup-logs/04-passive-rx.log
```

1. Put the board **in the place you intend to install it**.
2. Press a real speed button (e.g. Low, then Off) on the **OEM remote**,
   about five times, a couple of seconds apart. Write down which buttons you
   pressed, in order, with rough times.
3. Every received frame produces a line like:

   ```
   [qc.rx] RX #12: 6 bytes rssi=-71.5 dBm snr=… dB: CB.xx.xx.xx.yy.yy
   ```

4. Repeat from **next to the fan** and from **across the room**.

Record in the report: lines per press, frame lengths, the RSSI range at each
distance, and the `RX Valid Count` / `RX Rejected Count` values.

**Pass:** each press produces `[qc.rx]` frames beginning `CB` (remote/fan). `TX Count` stays **0**, because the controller is
unprovisioned and has no reason to transmit. `RX Valid Count` / `RX Rejected Count` may stay **0** here: on v0.3.1 they only
count once a remote is learned (step 5), so judge reception from the `[qc.rx]` lines.

**If RX is weak or missing** (few frames, many rejects, RSSI near −110 dBm):

1. Check the antenna is screwed fully onto the right connector.
2. Rebuild with the next wider filter and repeat this section, saving the log
   as `04b-passive-rx-78k.log`:

   ```bash
   .venv/bin/esphome -s rx_bandwidth 78_2kHz run quietcool-cpp-lora-v3-bringup.yaml
   ```

3. Report both results. A V3.2 board with weak RX is worth flagging, since
   hardware.md names that as the first suspect.

---

## 5. Learn the remote (pairing)

Follow [INSTALL.md §4](../INSTALL.md#4-pair-with-your-fan-learn-mode). Capture
the log as `05-learn.log`.

1. Press **Learn Remote ID** in HA.
2. Straight away, press a real speed button on the OEM remote about
   **three times, ~1 s apart**, within 60 s, while the fan is running.

**Pass:** `Remote Sender ID` shows `0xCB……`. The device log
shows the learn states (`learning_first`, `learning_second`) and then the bind. Reboot with the `Restart` button:
the ID must survive the reboot.

From here on the unit is **provisioned**. Shortly after every boot it sends one
non-energizing `66 66` status query, then one every 5 minutes.

---

## 6. Status query (first transmit, non-energizing)

Capture as `06-refresh.log`. Press **Refresh Fan State**.

**Pass:**

- `TX Count` goes up (one query burst).
- `[qc.rx]` lines follow within about a second: the fan's six-byte reply.
- `Fan State Known` → On, `Fan Evidence Source` = `manual_query`, and
  `Last Confirmed Fan State` matches what the fan is actually doing.
- If the fan is off, `Fan Confirmed Off` → On.

A query **cannot start the fan**. If the fan changes state at this step, stop
and report it straight away.

---

## 7. Closed-loop commands (energizing)

**Open a window first.** Capture as `07-commands.log`. Wait for each step to
settle (≈10 s) before the next. For each step, record what the fan **physically**
did, `Command Confirmation Status`, `Last TX Command`, and `TX Count`.

| Step | Action in HA | Expected physical result | Expected status |
| --- | --- | --- | --- |
| 7a | Fan → Low | Runs at low | `confirmed` |
| 7b | Fan → High | Runs at high | `confirmed` |
| 7c | Fan → Medium (only if `Fan Speed Capability` = 3) | Runs at medium | `confirmed` |
| 7d | Fan → Off | Stops | `confirmed`, `Fan Confirmed Off` On |
| 7e | `Fan Timer` → 1 hour | Starts at Low | `confirmed`, `Timer Remaining` ≈ 60 |
| 7f | Fan → Off | Stops | `confirmed` |
| 7g | Press Off three times quickly | Stays off | **One** transaction: `TX Count` rises by about one burst plus a query, not three |

Also record the value of `Fan Speed Capability` (2 or 3) and whether the
listed speeds in HA changed to match.

---

## 8. Coexistence with the OEM remote

Capture as `08-oem.log`. With the fan off, press **Low** on the OEM remote, then **Off**. On a two-button wall switch (e.g. IT-RFSWITCH-01: TIME cycles 1–12 h → On → Off, SPEED toggles H/L),
press TIME once to start the fan, SPEED once, then TIME until it reaches Off. These switches wake on the first press, so allow a few seconds per step.

**Pass:** the fan obeys every remote press. `Last TX Command` does not change
(the controller never sends a fan command of its own); `TX Count` rises only by its bounded, non-energizing `66 66` recovery queries. The HA fan entity follows,
or goes Unknown and resolves after its bounded recovery query.

---

## 9. Soak (24 hours)

Leave it running at the install location for at least 24 hours, with the
5-minute heartbeat on. Then record:

- `Uptime` (should be about 24 h, so no reboots) and `Reset Reason`
- `Controller Fault` (must be Off)
- `Loop Stack Min Free` (lowest value seen) and `Heap Free`
- `TX Count`, `RX Valid Count`, `RX Rejected Count`
- How many heartbeat queries got a reply. The HA history of
  `Fan State Known` should stay On.

Export the HA history graphs for those entities as screenshots if you can.

---

## 10. Report it

1. Fill in every section of [v3-bringup-report.md](v3-bringup-report.md).
2. Open an issue at
   <https://github.com/joyfulhouse/esphome-quietcool/issues> titled
   `V3 (SX1262) hardware bring-up report`.
3. Paste the filled-in report as the issue body. Attach the board photos, the
   OLED photo and the `bringup-logs/*.log` files.
4. **Before attaching logs, check them for anything private.** ESPHome logs
   show your Wi-Fi SSID and IP address. They never show the Wi-Fi password or
   API key, but scan them anyway. The fan's sender ID is a low-risk identifier;
   redact it if you prefer.
