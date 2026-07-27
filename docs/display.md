# OLED display

128×64 SSD1306, three zones:

- **Left** — an animated `mdi:fan` glyph (12 pre-rendered rotation frames, spin
  rate proportional to speed; a static `fan-off` glyph when off), the state word
  (OFF/LOW/MED/HIGH), and a timer countdown when a timer is running. The two
  builds differ here: the C++ core build (`quietcool-cpp-lora32.yaml`) shows
  **H:MM**, because the core publishes remaining *minutes* and only for a
  locally anchored, confirmed timer; the frozen legacy YAML build
  (`legacy/quietcool-lora32.yaml`) shows **HH:MM:SS**.
  On the legacy build, Learn mode also replaces the two text
  rows with `LEARN / REMOTE X2` (and briefly `LEARNED / ID SAVED` on success).
  The C++ core build (`components/quietcool/`) renders no learn prompt — its
  pairing progress is visible in the logs and the `Learn Remote ID` button
  entity instead.
- **Right** — the three temperatures, right-aligned, indoor dominant (22 pt), each
  with a small icon: house-thermometer (indoor), sun (outdoor), roof (attic).
  Missing/`NaN` values render `--`.
- **Bottom-left** — a status row of icons: WiFi signal strength, Home Assistant
  API connectivity, and battery.

The display renders the fan entity's latest query-confirmed state (or its raw
boot default before authority exists). Passive OEM traffic does not update this
safety-facing state. When that authority has been lost (`Fan State Known`
false — after boot, OEM remote activity, or an unconfirmed command), the state
word gains a `?` suffix (`HIGH?`, `OFF?`) so the panel never asserts a stale
word as current physical fact. Full closed-loop results are still published as
Home Assistant diagnostics (`Last Confirmed Fan State`, `Command Confirmation
Status`, `Fan Speed Capability`, `Fan State Known`, and atomic `Fan Confirmed
Off`). Do not read the displayed state word alone as an RF acknowledgement.

## Icon source

Glyphs come from the bundled Material Design Icons webfont
(`fonts/materialdesignicons-webfont.ttf`, unmodified). The fan-rotation bitmaps
are pre-rendered from the `mdi:fan` glyph by `tools/generate_fan_frames.py` into
`images/`.

## Preview renderer

`tools/render_display.py` is a pixel-accurate Pillow mirror of the **frozen
legacy** build's display lambda (every draw call is tagged `KEEP IN SYNC` in
both that file and `legacy/quietcool-lora32.yaml`). The zone geometry it
previews is shared with the C++ build, but its countdown is the legacy
HH:MM:SS one, so it is not a preview of the C++ build's H:MM row. It renders
the full state matrix into `docs/display-previews/` and asserts layout sanity
(no zone overlap, no clipping). Regenerate after display edits:

```bash
.venv/bin/python tools/generate_fan_frames.py   # if the fan icon changed
.venv/bin/python tools/render_display.py
```

## Two-tone panels

The running-state elements sit in the top rows; on the common two-tone SSD1306
variant (yellow top strip, blue below) that reads as a subtle color accent, and
degrades cleanly to monochrome on single-color panels.
