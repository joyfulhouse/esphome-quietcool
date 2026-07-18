# Firmware reverse-engineering

How the protocol was recovered. The 433.92 MHz carrier and 2-FSK modulation were
established first from SDR captures; everything else came from dumping and
disassembling the OEM handheld remote's microcontroller.

> The raw OEM firmware images are **not** redistributed in this repository (they
> are QuietCool's copyrighted code). Only independently derived facts —
> constants, offsets, and behavior — are documented below.

## The remote's MCU

- **STM32G030K6T6** (Cortex-M0+, 32 KiB flash), read-protection **RDP Level 0**
  (unprotected), so the flash reads out over SWD.
- Dump: `st-flash read fw.bin 0x08000000 0x8000` with an ST-Link (macOS needs
  `sudo` for USB access).

## Flash memory map (32 KiB)

| Region | Offset | Contents |
| --- | --- | --- |
| Vector table | `0x0000–0x00C0` | Cortex-M0+ handlers |
| Application | `0x00C0–0x6E00` | Main code |
| Button table | `0x6E00–0x6E20` | Button indices `01 02 03 04 06 07 08 09` |
| Device config | `0x6E20–0x6E50` | Sender-ID template + packet formats |
| RF register config | `0x6E50–0x6FD0` | CMT2300A-compatible register values |
| (erased) | `0x7000–0x7800` | `0xFF` padding |
| Runtime ID suffix | `0x7800–0x7802` | Per-unit ID bytes (little-endian) |

## Sender-ID mechanism

The compiled packet template carries `CB 00 01 52`. At boot the firmware
overwrites the last three bytes with the per-unit values at flash `0x7800`
(stored little-endian), producing the runtime sender ID. The universal `CB`
prefix is constant across all units.

## Command byte (confirmed by disassembly)

The RX status decoder at `0x08005F90` reads the command from the received
frame's bytes `[4]` and `[5]` (they must be equal) and extracts the speed field:

```asm
0x800602C: ldrb r3, [r3, #4]   ; command byte = frame[4]
0x800602E: lsls r3, r3, #2     ; bits 4,5 -> positions 6,7 ; bits 6,7 -> 8,9
0x8006030: uxtb r2, r3         ; & 0xFF drops positions 8,9 -> bits 6 AND 7 discarded
```

After the `uxtb`, byte bits 6 (`0x40`) and 7 (`0x80`) are physically discarded;
only bits 4–5 survive. So **speed = `(command >> 4) & 0x03`** (a 2-bit field) and
**duration = `command & 0x0F`**. The high-nibble `0x8` bit is a constant the
decoder ignores. See [protocol.md](protocol.md) for the resulting tables.

## Two remotes, one firmware

Two remotes (for two fans bought together) were dumped and compared byte-for-byte:

| | Remote A | Remote B |
| --- | --- | --- |
| Size | 32,768 B | 32,768 B |
| SHA-256 | `418dedb1…df66cef` | `1163edca…073cec00` |

They differ in **exactly three bytes**, all in the `0x7800–0x7802` per-unit ID
slot (`39 47 00` → runtime `CB 00 47 39`; `D3 D7 03` → runtime `CB 03 D7 D3`).
Everything else — code, button table, packet templates — is identical. There is
no per-fan firmware variant; only the ID is factory-burned.

### Confirmed: learn mode reads the real ID

This firmware's [learn mode](../README.md#learn-mode--porting-to-your-own-fan)
captured `CB 03 D7 D3` over the air from two remote presses — byte-for-byte what
the dump holds at `0x7800`. No dumping is needed to onboard a fan; the dump only
*confirmed* the over-the-air capture after the fact.

### The `80` vs `90` "Off" question

Remote B's Off button emits `80` where remote A's emits `90`. Since the firmware
is identical, this is not a model difference: `80` = *off, speed-index 0*; `90` =
*off, speed-index 1 (Low)* — a runtime remembered-speed difference in RAM, both
decoder-valid Off commands. It is not RF corruption/distance: the `80 80` frame
arrived with a perfect 4-byte sender match, equal command bytes, and correct
length, repeatedly — whereas genuine weak-signal noise showed up as malformed,
wrong-length frames. This firmware's RX accepts `0x80` as Off (the OEM decoder
masks the `0x80` bit away, so it accepts it too; the original strict `9/A/B`
whitelist was the only thing rejecting it).

## Tooling

Disassembly used `capstone` (Thumb / Cortex-M0+). Region-diff and ID-validation
were scripted against the two dumps. The scripts are kept out of this repo along
with the raw images.
