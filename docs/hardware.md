# Hardware

## Boards

### LilyGO TTGO LoRa32 V2.1 (433 MHz) — reference

- ESP32 + onboard **Semtech SX1278** (SX127x) + SSD1306 OLED.
- The board this project was developed and physically verified on.
- Buy: <https://amzn.to/4vBvqOU>

### Heltec / HiLetgo ESP32 LoRa V3 (SX1262)

- ESP32-**S3** + **Semtech SX1262** (SX126x) + SSD1306 OLED (Vext-gated).
- Newer, cheaper, more available. Uses `quietcool-lora-v3.yaml`.
- Buy: <https://amzn.to/4wagWqi> (includes a 433–510 MHz antenna).

The V3 config reproduces the exact same radio profile (2-FSK, 2400 bps, ±10 kHz,
sync `2D D4`, variable length) via ESPHome's `sx126x` component, and it compiles.
It is **not yet verified on hardware** — confirm these on first bring-up (all
flagged inline in the YAML as `PIN CONFIDENCE`):

| Item | Assumption | Confidence |
| --- | --- | --- |
| Radio pins (CS 8, BUSY 13, DIO1 14, RST 12, SPI 9/10/11) | ESPHome catalog + community | High |
| TCXO 1.8 V + DIO2 RF switch | required for SX1262 module | High |
| OLED (SDA 17, SCL 18, RST 21) + Vext GPIO36 (active-low) | catalog | High |
| Status-LED polarity (GPIO35) | pin known, polarity unconfirmed | Medium |
| VBAT ADC (GPIO1, enable GPIO37, ÷ 4.9) | community, untested | Medium |
| RX filter bandwidth (117.3 kHz) | closest FSK-legal to the V2.1 default | Tune on bench |

Some later "V3.2" units add a front-end module needing extra GPIO drive for good
RSSI; the base V3 design does not. If receive is weak, that's the first thing to
check.

<sub>Amazon Associate store `joyfulhousegi-20`; the maintainers may earn from
qualifying purchases.</sub>

## Antenna

**Always connect a 433 MHz antenna before transmitting** — keying a LoRa PA into
an open port can damage it. The SX127x/SX1262 are the working transceivers here;
OOK/ASK bridges (Sonoff RF Bridge and similar) cannot reproduce this 2-FSK link.

## Why not BLE?

QuietCool's *Smart* attic-fan line (`ATTICFAN*`) speaks JSON over BLE — a
completely different product and protocol. This project targets the **RF**
remote-controlled fans, which have no BLE.
