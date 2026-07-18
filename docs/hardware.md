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
