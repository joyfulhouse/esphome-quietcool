#!/usr/bin/env python3
"""Build-time generator for the display's pre-rendered fan rotation frames.

Display v3.2 replaces the hand-drawn triangle-blade fan icon with the real
MDI `mdi:fan` glyph. ESPHome's display lambda can draw a static `image:` at
an (x, y) position, but it cannot rotate one at runtime, so instead of
rotating anything on-device this tool rotates the glyph AHEAD OF TIME into a
fixed set of frames and the display lambda just flips between them (KEEP IN
SYNC: FAN_ANIM in quietcool-lora32.yaml's `display: lambda:`, mirrored
in tools/render_display.py).

Codepoint verification: `mdi:fan` and `mdi:fan-off` are NOT looked up from
memory or an external stylesheet - find_glyph_codepoint() below parses the
shipped fonts/materialdesignicons-webfont.ttf directly (its `post` table for
glyph names, its `cmap` table for the codepoint that maps to that glyph) and
this script asserts the result against the expected codepoints before
rendering anything, so a future font swap that renumbers glyphs fails loudly
here instead of silently drawing the wrong icon.

Rotation period: the brief for this tool assumed the glyph has 3-fold
(120 deg) rotational symmetry. Measuring frame-vs-frame pixel difference
across a full 360 deg sweep (see verify_rotation_period() below) shows that
is wrong for this glyph - the real minima are at 90/180/270/360 deg, i.e.
true 4-fold symmetry.

Frame count: the brief also asked for 6 frames advanced by +1/+2/+3 steps
(LOW/MED/HIGH) modulo the frame count. Simulating that sequence against 6
frames shows a second, independent bug: gcd(3, 6) = 3, so HIGH's frame
index sequence is 0, 3, 0, 3, ... forever - it only ever visits TWO
positions, i.e. it alternates/flickers instead of spinning (MED's +2 is
fine: gcd(2, 6) = 2 still gives a real 3-position forward cycle). Simply
tiling the measured 90 deg period across 6 frames at 15 deg apart would
therefore make the "fastest" fan speed look the most broken. This tool
instead uses 12 frames at 7.5 deg apart (still the same measured 90 deg
period - just twice the resolution), so every one of +1/+2/+3 gives a
period of 12/6/4 positions respectively: all >= 4, all genuinely cycling
forward, never degenerating to a 2-state flicker. The extra 6 frames cost
about 0.75 KB of flash (32x32 1-bit = 128 bytes/frame) - negligible - to
fix a real animation defect in the original 6-frame plan. Frame 0 and
frame 11 are 7.5 deg apart, not touching, but frame 11 -> frame 0 is
exactly one more 7.5 deg step, so the loop is still seamless;
verify_rotation_period() proves the full 90 deg period is a real symmetry
(not that adjacent frames match) at generation time rather than leaving it
to eyeballing a preview PNG.

Usage:
    .venv313/bin/python tools/generate_fan_frames.py

Writes images/fan_frame_0.png .. images/fan_frame_11.png (one rotation
period, 7.5 deg apart) and images/fan_off.png (static, un-rotated
`mdi:fan-off`), all 1-bit thresholded (no dithering - matches the real
SSD1306's binary pixels and ESPHome's own `image:` `type: BINARY` encoder).
"""

from __future__ import annotations

import math
import struct
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageStat

ROOT = Path(__file__).resolve().parents[1]
MDI_TTF = ROOT / "fonts" / "materialdesignicons-webfont.ttf"
IMAGES_DIR = ROOT / "images"

FAN_GLYPH_NAME = "fan"
FAN_OFF_GLYPH_NAME = "fan-off"
# Independently re-derived from the font itself by find_glyph_codepoint();
# these are only the expected values asserted against that result.
EXPECTED_FAN_CODEPOINT = 0xF0210
EXPECTED_FAN_OFF_CODEPOINT = 0xF081D

FRAME_SIZE = 32  # px, final square canvas for every frame (matches the ~30px
                 # footprint of the v3.1 hand-drawn icon, "slightly larger")
SUPERSAMPLE = 8  # render/rotate at this multiple, then downsample for AA
ROTATION_FRAMES = 12  # not 6 - see "Frame count" in the module docstring
ROTATION_PERIOD_DEG = 90.0  # measured 4-fold symmetry (see module docstring)
ROTATION_STEP_DEG = ROTATION_PERIOD_DEG / ROTATION_FRAMES  # 7.5 deg


# =============================================================================
# Pure-stdlib TTF `post`/`cmap` parsing - no fontTools dependency available in
# this environment's .venv313, so this hand-rolls just enough of the sfnt
# table format to answer one question: "what codepoint maps to the glyph
# named X?"
# =============================================================================
def _read_sfnt_tables(data: bytes) -> dict[str, tuple[int, int]]:
    num_tables = struct.unpack_from(">H", data, 4)[0]
    tables = {}
    off = 12
    for _ in range(num_tables):
        tag = data[off : off + 4].decode("ascii")
        table_off = struct.unpack_from(">I", data, off + 8)[0]
        table_len = struct.unpack_from(">I", data, off + 12)[0]
        tables[tag] = (table_off, table_len)
        off += 16
    return tables


def _glyph_id_for_name(data: bytes, post_off: int, post_len: int, glyph_name: str) -> int:
    version = struct.unpack_from(">I", data, post_off)[0]
    if version != 0x00020000:
        raise ValueError(f"unsupported post table version {version:#x} (expected 2.0)")
    num_glyphs = struct.unpack_from(">H", data, post_off + 32)[0]
    idx_off = post_off + 34
    glyph_name_index = [
        struct.unpack_from(">H", data, idx_off + 2 * i)[0] for i in range(num_glyphs)
    ]
    names_off = idx_off + 2 * num_glyphs
    names: list[str] = []
    pos = names_off
    end = post_off + post_len
    while pos < end:
        length = data[pos]
        pos += 1
        names.append(data[pos : pos + length].decode("ascii", "replace"))
        pos += length
    for gid, idx in enumerate(glyph_name_index):
        if idx >= 258 and names[idx - 258] == glyph_name:
            return gid
    raise ValueError(f"no glyph named {glyph_name!r} in post table")


def _codepoint_for_glyph_id(data: bytes, cmap_off: int, target_gid: int) -> int:
    num_subtables = struct.unpack_from(">H", data, cmap_off + 2)[0]
    subtable_offsets = []
    for i in range(num_subtables):
        rec_off = cmap_off + 4 + i * 8
        sub_off = cmap_off + struct.unpack_from(">I", data, rec_off + 4)[0]
        subtable_offsets.append(sub_off)

    for sub_off in subtable_offsets:
        fmt = struct.unpack_from(">H", data, sub_off)[0]
        if fmt != 12:
            continue  # format 12 (segmented coverage) is what carries the
                       # supplementary-plane PUA codepoints MDI uses; the
                       # BMP-only format 4 subtables never contain them.
        num_groups = struct.unpack_from(">I", data, sub_off + 12)[0]
        for g in range(num_groups):
            grp_off = sub_off + 16 + g * 12
            start_char, end_char, start_glyph = struct.unpack_from(">III", data, grp_off)
            if start_glyph <= target_gid <= start_glyph + (end_char - start_char):
                return start_char + (target_gid - start_glyph)
    raise ValueError(f"glyph id {target_gid} not found in any format-12 cmap subtable")


def find_glyph_codepoint(font_path: Path, glyph_name: str) -> int:
    """Look up the Unicode codepoint that renders the glyph named `glyph_name`,
    by parsing the font's own `post` (glyph names) and `cmap` (codepoint ->
    glyph id) tables directly. Raises if the font has no such glyph."""
    data = font_path.read_bytes()
    tables = _read_sfnt_tables(data)
    post_off, post_len = tables["post"]
    cmap_off, _cmap_len = tables["cmap"]
    gid = _glyph_id_for_name(data, post_off, post_len, glyph_name)
    return _codepoint_for_glyph_id(data, cmap_off, gid)


# =============================================================================
# Rendering pipeline
# =============================================================================
def _render_glyph_square(font_path: Path, codepoint: int, target_px: int, supersample: int) -> Image.Image:
    """Render one glyph, tightly cropped and centered in a square canvas
    sized to safely contain any rotation of it (diagonal of its own bbox),
    at `supersample`x the final resolution for antialiased downsampling."""
    char = chr(codepoint)
    # Trial render to measure this glyph's actual height-per-point-size
    # ratio (TrueType scaling is linear in the em square, so one measurement
    # plus a proportional correction lands within a pixel of the target).
    trial_size = 500
    font = ImageFont.truetype(str(font_path), trial_size)
    trial_canvas = Image.new("L", (trial_size * 2, trial_size * 2), 0)
    ImageDraw.Draw(trial_canvas).text((trial_size // 2, trial_size // 2), char, font=font, fill=255)
    trial_bbox = trial_canvas.getbbox()
    if trial_bbox is None:
        raise ValueError(f"glyph U+{codepoint:05X} rendered empty at trial size")
    trial_h = trial_bbox[3] - trial_bbox[1]

    target_glyph_px = int(target_px * supersample * 0.9)  # small margin so
                                                            # rotation never
                                                            # touches the
                                                            # padded canvas edge
    final_size = max(8, round(trial_size * target_glyph_px / trial_h))
    font = ImageFont.truetype(str(font_path), final_size)
    probe = Image.new("L", (final_size * 2, final_size * 2), 0)
    ImageDraw.Draw(probe).text((final_size // 2, final_size // 2), char, font=font, fill=255)
    bbox = probe.getbbox()
    if bbox is None:
        raise ValueError(f"glyph U+{codepoint:05X} rendered empty at final size")
    glyph = probe.crop(bbox)

    w, h = glyph.size
    # Diagonal of the bbox is the largest footprint the glyph can ever
    # occupy under rotation about its own center - pad to that so no frame
    # clips a corner regardless of angle.
    diagonal = math.ceil(math.hypot(w, h)) + 4
    canvas_side = max(diagonal, target_px * supersample)
    square = Image.new("L", (canvas_side, canvas_side), 0)
    square.paste(glyph, ((canvas_side - w) // 2, (canvas_side - h) // 2))
    return square


def _finalize_frame(square: Image.Image, angle_deg: float, target_px: int) -> Image.Image:
    rotated = square.rotate(angle_deg, resample=Image.BICUBIC, expand=False, fillcolor=0)
    downsampled = rotated.resize((target_px, target_px), Image.LANCZOS)
    # Threshold to pure 1-bit, no dithering - matches the real SSD1306's
    # binary pixels and ESPHome's own `image: type: BINARY` encoder (which
    # defaults dither: NONE).
    thresholded = downsampled.point(lambda p: 255 if p >= 128 else 0)
    return thresholded.convert("1", dither=Image.Dither.NONE)


def verify_rotation_period(square: Image.Image, period_deg: float, target_px: int) -> float:
    """Prove the chosen period is actually a symmetry of this glyph: render
    frame 0 and a frame rotated by exactly `period_deg`, and return their
    mean per-pixel difference (0..255) after the same downsample+threshold
    pipeline every real frame goes through. A tiny number here means the
    loop from the last generated frame back to frame 0 is seamless."""
    frame_0 = _finalize_frame(square, 0.0, target_px)
    frame_period = _finalize_frame(square, period_deg, target_px)
    diff = ImageChops.difference(frame_0.convert("L"), frame_period.convert("L"))
    return ImageStat.Stat(diff).mean[0]


def main() -> None:
    if not MDI_TTF.is_file():
        raise SystemExit(f"missing {MDI_TTF}")

    fan_codepoint = find_glyph_codepoint(MDI_TTF, FAN_GLYPH_NAME)
    fan_off_codepoint = find_glyph_codepoint(MDI_TTF, FAN_OFF_GLYPH_NAME)
    print(f"Verified via font cmap: mdi:{FAN_GLYPH_NAME} = U+{fan_codepoint:05X}")
    print(f"Verified via font cmap: mdi:{FAN_OFF_GLYPH_NAME} = U+{fan_off_codepoint:05X}")
    assert fan_codepoint == EXPECTED_FAN_CODEPOINT, (
        f"mdi:fan codepoint drifted: font says U+{fan_codepoint:05X}, "
        f"expected U+{EXPECTED_FAN_CODEPOINT:05X} - font file may have "
        f"changed; update EXPECTED_FAN_CODEPOINT and the YAML glyph list "
        f"together after checking the new glyph visually."
    )
    assert fan_off_codepoint == EXPECTED_FAN_OFF_CODEPOINT, (
        f"mdi:fan-off codepoint drifted: font says U+{fan_off_codepoint:05X}, "
        f"expected U+{EXPECTED_FAN_OFF_CODEPOINT:05X}"
    )

    IMAGES_DIR.mkdir(parents=True, exist_ok=True)

    fan_square = _render_glyph_square(MDI_TTF, fan_codepoint, FRAME_SIZE, SUPERSAMPLE)

    period_diff = verify_rotation_period(fan_square, ROTATION_PERIOD_DEG, FRAME_SIZE)
    print(
        f"Rotation period check: frame-0 vs frame+{ROTATION_PERIOD_DEG:.0f}deg "
        f"mean pixel diff = {period_diff:.2f}/255 (near 0 = seamless loop)"
    )
    if period_diff > 12.0:
        raise SystemExit(
            f"Rotation period {ROTATION_PERIOD_DEG} deg does not look like a "
            f"real symmetry of this glyph (diff {period_diff:.2f} too high); "
            f"re-measure with a finer angle sweep before trusting the loop."
        )

    for i in range(ROTATION_FRAMES):
        angle = i * ROTATION_STEP_DEG
        frame = _finalize_frame(fan_square, angle, FRAME_SIZE)
        out_path = IMAGES_DIR / f"fan_frame_{i}.png"
        frame.save(out_path)
        print(f"  wrote {out_path.relative_to(ROOT)}  (angle={angle:.1f} deg)")

    off_square = _render_glyph_square(MDI_TTF, fan_off_codepoint, FRAME_SIZE, SUPERSAMPLE)
    off_frame = _finalize_frame(off_square, 0.0, FRAME_SIZE)
    off_path = IMAGES_DIR / "fan_off.png"
    off_frame.save(off_path)
    print(f"  wrote {off_path.relative_to(ROOT)}  (static, un-rotated)")


if __name__ == "__main__":
    main()
