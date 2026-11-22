# V3 (SX1262) hardware bring-up report

<!-- Fill in each field; use PASS / FAIL / N/A plus a note. Steps refer to
docs/v3-bringup.md. Attach photos and bringup-logs/*.log to the issue. -->

## Setup

| Field | Value |
| --- | --- |
| Board as sold | HiLetgo ESP32 LoRa V3 (SX1262), Amazon link in docs/hardware.md |
| Silkscreen version (V3 / V3.1 / V3.2) | |
| esptool chip / flash size (step 1) | |
| ESPHome version | |
| Repository commit (`git rev-parse --short HEAD`) | |
| Config built | `quietcool-cpp-lora-v3-bringup.yaml`, `rx_bandwidth` = |
| Antenna | (the one in the kit / other) |
| Fan model, speeds | |
| Distance and obstacles between board and fan | |

## 2. Flash and first boot — PASS / FAIL

- [ ] Compiled and flashed over USB
- [ ] Joined Wi-Fi and was adopted by Home Assistant
- [ ] `SX126x:` config dump present, no `Unsupported chip version` / `Wait busy timeout`
- [ ] No `QuietCool controller degraded`; `Controller Fault` = Off

Notes:

## 3. Board peripherals

| # | Item | Result | Observed |
| --- | --- | --- | --- |
| 3a | I2C scan finds 0x3C | | |
| 3b | OLED test pattern visible (Vext GPIO36 active-low, SDA17/SCL18/RST21) | | |
| 3c | Status LED GPIO35 polarity | active-high / active-low | |
| 3d | PRG button GPIO0 | | |
| 3e | VBAT raw, no battery | | V |
| 3f | VBAT x4.9 vs multimeter | | ESP: V, meter: V |
| 3g | VBAT enable GPIO37 polarity | active-low / active-high | |

## 4. Passive receive — PASS / FAIL

| Location | Presses | `[qc.rx]` lines / press | Frame lengths | RSSI range (dBm) |
| --- | --- | --- | --- | --- |
| Install location | | | | |
| Next to fan | | | | |
| Across room | | | | |

- RX Valid Count: ______  RX Rejected Count: ______  TX Count (should be 0): ______
- Repeated at 78_2kHz? yes / no — result:

## 5. Learn — PASS / FAIL

- Remote Sender ID: `0xCB______` (redact last bytes if you prefer)
- Presses needed: ______  ID survived reboot: yes / no

## 6. Status query — PASS / FAIL

- TX Count before / after: ______ / ______
- Fan reply seen in log: yes / no  Fan State Known: ______  Evidence Source: ______
- Last Confirmed Fan State vs. physical: ______
- Fan changed state on a query (must be NO): ______

## 7. Commands

`Fan Speed Capability`: ______ (HA speed list updated to match: yes / no)

| Step | Physical result | Command Confirmation Status | Last TX Command | TX Count after |
| --- | --- | --- | --- | --- |
| 7a Low | | | | |
| 7b High | | | | |
| 7c Medium (3-speed only) | | | | |
| 7d Off | | | | |
| 7e Timer 1 h | | Timer Remaining: | | |
| 7f Off | | | | |
| 7g 3× rapid Off | | | | |

## 8. OEM remote coexistence — PASS / FAIL

- Fan obeyed remote: ______  TX Count flat during presses: ______
- HA entity followed / went Unknown then resolved: ______

## 9. 24 h soak — PASS / FAIL

| Metric | Value |
| --- | --- |
| Uptime at end | |
| Reset Reason | |
| Controller Fault | |
| Loop Stack Min Free (lowest) | B |
| Heap Free (lowest) | B |
| TX / RX valid / RX rejected | |
| Fan State Known stayed On | |

## Anything else

<!-- Unexpected log warnings, crashes, odd behaviour, photos referenced. -->
