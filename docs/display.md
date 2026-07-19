# OLED display

128×64 SSD1306, three zones:

- **Left** — an animated `mdi:fan` glyph (12 pre-rendered rotation frames, spin
  rate proportional to speed; a static `fan-off` glyph when off), the state word
  (OFF/LOW/MED/HIGH), and an **HH:MM:SS** timer countdown when a timer is running.
  Learn mode replaces the two text rows with `LEARN / REMOTE X2`.
- **Right** — the three temperatures, right-aligned, indoor dominant (22 pt), each
  with a small icon: house-thermometer (indoor), sun (outdoor), roof (attic).
  Missing/`NaN` values render `--`.
- **Bottom-left** — a status row of icons: WiFi signal strength, Home Assistant
  API connectivity, and battery.

The display renders the public template's local/mirrored fan entity state. It
does **not** currently indicate that a command was confirmed by a queried fan.
The closed-loop confirmation and capability diagnostics described in the
protocol research belong to a live-validated downstream implementation that has
not yet been ported into the public YAML or preview renderer; do not read the
displayed state word as an RF acknowledgement.

## Icon source

Glyphs come from the bundled Material Design Icons webfont
(`fonts/materialdesignicons-webfont.ttf`, unmodified). The fan-rotation bitmaps
are pre-rendered from the `mdi:fan` glyph by `tools/generate_fan_frames.py` into
`images/`.

## Preview renderer

`tools/render_display.py` is a pixel-accurate Pillow mirror of the display
lambda (every draw call is tagged `KEEP IN SYNC` in both files). It renders the
full state matrix into `docs/display-previews/` and asserts layout sanity (no
zone overlap, no clipping). Regenerate after display edits:

```bash
.venv/bin/python tools/generate_fan_frames.py   # if the fan icon changed
.venv/bin/python tools/render_display.py
```

## Two-tone panels

The running-state elements sit in the top rows; on the common two-tone SSD1306
variant (yellow top strip, blue below) that reads as a subtle color accent, and
degrades cleanly to monochrome on single-color panels.
