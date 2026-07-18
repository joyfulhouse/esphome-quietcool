# QuietCool RF protocol

Independently reverse-engineered from the OEM handheld remote (STM32 firmware
dump + SDR captures). All values below are locally verified against real fans.

## Radio profile

| Setting | Value |
| --- | --- |
| Carrier | 433,920,000 Hz |
| Modulation | 2-FSK (GFSK, pulse-shaping off) |
| Bit rate | 2,400 bit/s |
| Deviation | ±10,000 Hz |
| Preamble | Eight `AA` bytes |
| Sync word | `2D D4` |
| Length | Variable; the packet engine inserts/strips a `06` length byte |
| CRC / whitening / Manchester / FEC | All disabled |
| TX power | +17 dBm (BOOST on the SX1278) |
| Repetition | Three frames, 45 ms after each transmission |

## Frame

The transceiver's packet engine supplies the preamble, sync, and length byte.
Firmware supplies only the six-byte application payload:

```
AA AA AA AA AA AA AA AA | 2D D4 | 06 | SS SS SS SS | CMD CMD
                                        └ 4-byte sender ID  └ command, sent twice
```

- **Sender ID** — four bytes, always beginning `CB`. Per-unit; see
  [firmware-analysis.md](firmware-analysis.md). The two command bytes are
  identical and a receiver rejects the frame if they differ.

## Command byte

`command = (0x8 | speed_index) << 4 | duration_nibble`

The high nibble is `0x8 | speed_index`; the decoder extracts speed as
`(command >> 4) & 0x03` — a **2-bit field** — so the `0x8` bit is a constant it
discards, and the field saturates at High (there is no over-High speed).

| High nibble | speed_index | Speed |
| --- | --- | --- |
| `8` | 0 | off / no remembered speed |
| `9` | 1 | Low |
| `A` | 2 | Medium |
| `B` | 3 | High |

| Low nibble | Meaning |
| --- | --- |
| `0` | Off |
| `1` / `2` / `4` / `8` | 1 / 2 / 4 / 8 hours |
| `C` | 12 hours |
| `F` | Continuous on |

Examples: `9F` Low continuous · `AF` Medium · `BF` High · `B1` High for 1 h ·
`90` off (Low remembered) · `80` off (no remembered speed). Because the decoder
masks to bits 4–5, high nibbles `C`–`F` alias onto `8`–`B`; a real remote only
emits `8`/`9`/`A`/`B`.

## Wake / status query

The OEM remote also transmits `SS SS SS SS 66 66` (a wake/status query) before
some interactions and then listens. This firmware logs received `66 66` frames
but does not itself transmit the query; authoritative query-based state is
future work.
