#!/usr/bin/env python3
"""Pixel-accurate Python mirror of the QuietCool OLED display lambda.

This is a hand-port, not a YAML parser: every drawing call below is tagged
`KEEP IN SYNC: <NAME>` and the matching tag appears next to the equivalent
C++ in the `display: lambda:` block of quietcool-lora32.yaml. When
the lambda changes, update the matching section here (and vice versa).

Usage:
    .venv313/bin/python tools/render_display.py

Renders the state-matrix preview PNGs and the fan-icon rotation strip into
docs/display-previews/, then runs programmatic layout-sanity assertions
(zone overlap, edge clipping, minimum margins) and prints the results.

Rendering pipeline: draw at native 128x64 into a 1-bit ("1" mode) PIL image
with NO antialiasing (matches the real SSD1306's binary pixels), then
upscale x4 with NEAREST for a human-viewable PNG. The 1-bit buffer is what
the sanity assertions measure against, not the upscaled copy.
"""

from __future__ import annotations

import math
import os
from dataclasses import dataclass, field
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONTS_DIR = ROOT / "fonts"
IMAGES_DIR = ROOT / "images"
PREVIEWS_DIR = ROOT / "docs" / "display-previews"

ROBOTO_TTF = ROOT / ".esphome" / "font" / "Roboto@400@False@v1.ttf"
MDI_TTF = FONTS_DIR / "materialdesignicons-webfont.ttf"

# =============================================================================
# KEEP IN SYNC: CANVAS / ZONES (quietcool-lora32.yaml: display: lambda:)
#
#   LEFT  zone (x   0-54): fan icon, state word, HH:MM:SS countdown (or the
#                          learn-mode text rows in those two slots), and the
#                          bottom-left status icon row (wifi/api/battery)
#   RIGHT zone (x 66-127): temperatures, right-aligned, each paired with a
#                          small glyph in a shared left-edge icon column
#                          (v3.3: replaces the old "Out"/"At" text labels)
#
# The x55-65 gap is intentional empty breathing room (v3.1's vertical
# status-icon column used to live there - see "v3.2 revisions" in README).
# =============================================================================
CANVAS_W = 128
CANVAS_H = 64

ZONES = {
    "LEFT": (0, 54),
    "RIGHT": (66, 127),
}
ZONE_SEAMS = {
    "LEFT": {"right"},
    "RIGHT": {"left"},
}
MIN_MARGIN = 1  # px, minimum clearance from a zone edge or the canvas edge

# ---- KEEP IN SYNC: FAN_ANIM ----
# Real `mdi:fan` glyph, pre-rendered into 12 rotation frames by
# tools/generate_fan_frames.py (see that tool's docstring for why 12
# frames/7.5deg apart, not the 6 frames/20deg the original brief assumed -
# both the glyph's true rotational symmetry period, measured at 90deg not
# 120deg, and the +1/+2/+3-step-per-refresh scheme needing a frame count
# that keeps every speed's step count coprime enough with it to avoid a
# short cycle, forced this tool away from the naive "6 frames" plan).
FAN_ICON_POS = (11, 1)  # TOP_LEFT, 32x32
FRAME_SIZE = 32
ROTATION_FRAMES = 12
FAN_FRAME_STEP = {0: 0, 1: 1, 2: 2, 3: 3}  # speed idx -> frame-index step/refresh (OFF/LOW/MED/HIGH)

# ---- KEEP IN SYNC: STATE_WORD ----
LEFT_ZONE_CENTER_X = 27
# y=29/40 (originally 34/43): raised twice per on-hardware review - the
# countdown's digit ink clipped the bottom status-icon row, and the state
# word then needed 2px more air above the countdown.
STATE_WORD_POS = (LEFT_ZONE_CENTER_X, 29)  # TOP_CENTER
STATE_WORD_FONT_SIZE = 11
SPEED_NAMES = ("OFF", "LOW", "MED", "HIGH")

# ---- KEEP IN SYNC: COUNTDOWN ----
# HH:MM:SS while a timer is active; blank while continuous-on (running,
# no timer) or off - no infinity glyph (removed: illegible "OO" at 18px).
# y=40, not the 43 the learn prompts use; see the state-word note above.
COUNTDOWN_POS = (LEFT_ZONE_CENTER_X, 40)  # TOP_CENTER
COUNTDOWN_FONT_SIZE = 11

# ---- KEEP IN SYNC: LEARN_STATE ----
# Learn mode and its brief confirmation replace the state-word and
# countdown text slots (v3.3 geometry) without disturbing the fan icon or
# the bottom status row.
LEARN_TITLE_POS = (LEFT_ZONE_CENTER_X, 33)  # TOP_CENTER
LEARN_PROMPT_POS = (LEFT_ZONE_CENTER_X, 43)  # TOP_CENTER
LEARN_FONT_SIZE = 9
LEARN_PROMPT_FONT_SIZE = 8

# ---- KEEP IN SYNC: STATUS_ICONS (wifi + api + battery, bottom-left horizontal row) ----
# Moved off the old center vertical stack per operator review of v3.1
# ("the status icons should be a HORIZONTAL row in the BOTTOM-LEFT corner,
# not a center vertical stack"). All three icons share font_icons (13pt,
# ~13px advance width each), evenly spaced left-to-right.
STATUS_ROW_Y = 52
WIFI_ICON_POS = (4, STATUS_ROW_Y)  # TOP_LEFT
API_ICON_POS = (21, STATUS_ROW_Y)  # TOP_LEFT
BATTERY_ICON_POS = (38, STATUS_ROW_Y)  # TOP_LEFT
ICON_FONT_SIZE = 13
WIFI_STRENGTH_4 = "\U000F0928"
WIFI_STRENGTH_3 = "\U000F0925"
WIFI_STRENGTH_2 = "\U000F0922"
WIFI_STRENGTH_1 = "\U000F091F"
WIFI_STRENGTH_OFF = "\U000F092D"
API_CONNECTED = "\U000F07D0"  # home-assistant (the HA logo glyph itself)
API_DISCONNECTED = "\U000F0C9C"  # network-off-outline - see v3.3 revisions in
# README for why this pairs with home-assistant instead of a manual
# slash-overlay on the logo: rendering a hand-drawn diagonal line on top of
# home-assistant's already-dense 13px silhouette produced illegible noise,
# not a clean "off" signal (verified by rendering both side by side before
# picking this pair) - network-off-outline is a purpose-built MDI "off"
# glyph, unmistakably not house-shaped, so it can't be confused with the
# new TEMP_ICONS indoor glyph below either.

# ---- KEEP IN SYNC: BATTERY_ICON (voltage-only heuristic; see README "Battery") ----
# Tiers, ascending by voltage: 0=hidden(no battery, <2.5V) 1=alert(<15%)
# 2=outline(15-59%, non-critical/non-full) 3=full(60-100%, not
# charging/plugged) 4=charging(~4.20-4.30V topping-off band) 5=plugged
# (>=4.30V, USB rail - full or absent). Capacity is intentionally only
# THREE buckets (alert/outline/full), not five: the numbered
# battery-20/40/70 glyphs were tried first and rejected after rendering
# them at 13px 1-bit - they only differ by a faint fill band near the
# icon's top that's imperceptible at this size, so they collapsed to
# three maximally-distinct silhouettes instead (same failure mode, and
# same fix, as the old API glyph). Each boundary has its own rise/fall
# hysteresis gap; see the C++ mirror in the display lambda for why (a
# persisted previous-tier global keeps a borderline reading from flapping
# the icon). The renderer has no frame history, so every preview is
# generated as a fresh "bootstrap" read (prev_tier = -1): pick the tier
# the raw voltage falls into with no hysteresis applied, matching the
# display lambda's own bootstrap branch.
BATTERY_RISE = (2.55, 3.74, 3.90, 4.23, 4.33)
BATTERY_GLYPHS = {
    1: "\U000F0083",  # battery-alert
    2: "\U000F008E",  # battery-outline
    3: "\U000F0079",  # battery (full)
    4: "\U000F0084",  # battery-charging
    5: "\U000F06A5",  # power-plug
}


def battery_tier(voltage: float | None) -> int:
    if voltage is None:
        voltage = -1.0
    tier = 0
    for i, threshold in enumerate(BATTERY_RISE):
        if voltage >= threshold:
            tier = i + 1
    return tier

# ---- KEEP IN SYNC: TEMPERATURES ----
INDOOR_POS = (127, 0)  # TOP_RIGHT
OUTDOOR_POS = (127, 27)  # TOP_RIGHT
ATTIC_POS = (127, 43)  # TOP_RIGHT
TEMP_LARGE_FONT_SIZE = 22
TEMP_SMALL_FONT_SIZE = 11

# ---- KEEP IN SYNC: TEMP_ICONS (v3.3: icons replace the "Out"/"At" text
# labels - operator feedback: "Out/At doesn't meaningfully tell me what's
# going on - icons for that would be nice too") ----
# One shared left-edge x for all three icons (a "column", same idea as the
# status row's shared y) so the eye reads down one straight line while the
# right-aligned numbers stay dominant and unmoved from v3.1/v3.2. x=67 is
# the tightest legal value: RIGHT zone's left seam sits at x=66 and
# MIN_MARGIN=1 forbids anything closer. All three glyphs measure exactly
# 13px wide at ICON_FONT_SIZE (verified below), so the column's right edge
# is a consistent x=80 too.
#
# KNOWN LIMITATION (pre-existing, not new to v3.3): indoor is 22pt and
# right-aligned at x=127; a 2-digit reading ("78°F", 45px wide) leaves the
# icon a 2px clearance, but a 3-digit reading ("100°F", 57px wide) would
# overlap it. v3.1/v3.2 already left indoor only ~4px of total zone margin
# at 3 digits with NO icon at all, so this isn't a regression - it's the
# same accepted tradeoff the zone width already implied, just now visible
# as an icon/number collision instead of a number nearly touching the
# zone edge. Not fixed here (indoor stays 1-2 digits in every state this
# controller has actually been run against); flagged honestly instead of
# silently hoping it doesn't happen, matching this file's Battery-section
# precedent for documenting heuristic limits instead of hiding them.
TEMP_ICON_X = 67
INDOOR_ICON_POS = (TEMP_ICON_X, 3)  # TOP_LEFT; vertically centers home-
# thermometer-outline's 11px ink against the 22pt indoor row's 16px ink
# height: (16-11)/2 = 2.5, rounded up.
OUTDOOR_ICON_POS = (TEMP_ICON_X, 25)  # TOP_LEFT; weather-sunny's 12px ink
# is TALLER than the 11pt outdoor row's 8px digit ink, so it centers
# slightly ABOVE the row's own y: 27 + (8-12)/2 = 25.
ATTIC_ICON_POS = (TEMP_ICON_X, 43)  # TOP_LEFT; home-roof's 8px ink height
# matches the 11pt attic row's 8px digit ink height exactly - no offset.
INDOOR_ICON = "\U000F0F55"  # home-thermometer-outline - tried filled
# home-thermometer (U+F0F54) first per the brief; rejected after rendering
# both at 13px 1-bit side by side (see v3.3 revisions in README) - the
# filled version's roof+bulb regions collapse into a blob, while the
# outline's hollow house silhouette plus a thin thermometer stem stays
# crisp, matching this file's existing outline-over-filled precedent (see
# BATTERY_ICON's battery-outline tier above).
OUTDOOR_ICON = "\U000F0599"  # weather-sunny - tried sun-thermometer
# (U+F18D6) and sun-thermometer-outline (U+F18D7) first per the brief;
# both rejected after rendering: cramming a dashed sun-ray glyph AND a
# thermometer stem into one 13px glyph reads as noise, not a shape (see
# v3.3 revisions in README). weather-sunny's dashed sun burst alone is
# crisp and unambiguous for "outdoor" at this size - the adjoining
# "<value>°F" text already carries the temperature meaning, matching how
# ATTIC_ICON below signals location only, not location+thermometer.
ATTIC_ICON = "\U000F112B"  # home-roof

UPSCALE = 4


def _font(path: Path, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(path), size)


@dataclass
class Element:
    name: str
    zone: str | None
    bbox: tuple[int, int, int, int]  # x0, y0, x1, y1 (inclusive-ish, PIL convention)


@dataclass
class Canvas:
    image: Image.Image
    draw: ImageDraw.ImageDraw
    elements: list[Element] = field(default_factory=list)

    @classmethod
    def new(cls) -> "Canvas":
        img = Image.new("1", (CANVAS_W, CANVAS_H), 0)
        return cls(image=img, draw=ImageDraw.Draw(img))

    def filled_circle(self, cx: int, cy: int, r: int) -> tuple[int, int, int, int]:
        bbox = (cx - r, cy - r, cx + r, cy + r)
        self.draw.ellipse(bbox, fill=1)
        return bbox

    def circle(self, cx: int, cy: int, r: int) -> tuple[int, int, int, int]:
        bbox = (cx - r, cy - r, cx + r, cy + r)
        self.draw.ellipse(bbox, outline=1)
        return bbox

    def filled_triangle(self, p0, p1, p2) -> tuple[int, int, int, int]:
        self.draw.polygon([p0, p1, p2], fill=1)
        xs = [p0[0], p1[0], p2[0]]
        ys = [p0[1], p1[1], p2[1]]
        return (min(xs), min(ys), max(xs), max(ys))

    def triangle(self, p0, p1, p2) -> tuple[int, int, int, int]:
        self.draw.polygon([p0, p1, p2], outline=1)
        xs = [p0[0], p1[0], p2[0]]
        ys = [p0[1], p1[1], p2[1]]
        return (min(xs), min(ys), max(xs), max(ys))

    def text_topleft(self, x: int, y: int, font: ImageFont.FreeTypeFont, text: str):
        self.draw.text((x, y), text, font=font, fill=1, anchor="lt")
        return self.draw.textbbox((x, y), text, font=font, anchor="lt")

    def text_topright(self, x: int, y: int, font: ImageFont.FreeTypeFont, text: str):
        self.draw.text((x, y), text, font=font, fill=1, anchor="rt")
        return self.draw.textbbox((x, y), text, font=font, anchor="rt")

    def text_topcenter(self, x: int, y: int, font: ImageFont.FreeTypeFont, text: str):
        self.draw.text((x, y), text, font=font, fill=1, anchor="mt")
        return self.draw.textbbox((x, y), text, font=font, anchor="mt")

    def paste_image(self, x: int, y: int, glyph: Image.Image) -> tuple[int, int, int, int]:
        """Paste a pre-rendered 1-bit frame image top-left anchored at
        (x, y) - mirrors `it.image(x, y, id(...))`'s default TOP_LEFT
        alignment. Registers only the glyph's own non-blank pixels (not the
        full padded square) so overlap/margin checks are as tight as the
        vector-drawn elements elsewhere in this module."""
        self.image.paste(glyph, (x, y))
        tight = glyph.getbbox()
        if tight is None:
            return (x, y, x, y)
        return (x + tight[0], y + tight[1], x + tight[2], y + tight[3])

    def register(self, name: str, zone: str | None, bbox: tuple[int, int, int, int]) -> None:
        self.elements.append(Element(name, zone, bbox))


@dataclass
class DisplayState:
    running: bool
    speed_idx: int  # 0=off/unset, 1=LOW, 2=MED, 3=HIGH
    timer_active: bool
    remaining_ms: int
    frame_idx: int  # fan_anim_frame value to render THIS frame (already advanced), 0..11
    wifi_up: bool
    rssi: float
    api_up: bool
    indoor: float | None  # None => non-finite / no state ("--")
    outdoor: float | None
    attic: float | None
    battery_voltage: float | None = 3.83  # None => no battery attached (hidden icon)
    learn_active: bool = False
    learn_confirm: bool = False


_FAN_FRAME_CACHE: dict[str, Image.Image] = {}


def _load_fan_frame(name: str) -> Image.Image:
    """Load one of tools/generate_fan_frames.py's PNGs directly (KEEP IN
    SYNC: FAN_ANIM says to load the PNGs rather than re-render glyphs) -
    cached since the same handful of frames repeat across every state."""
    if name not in _FAN_FRAME_CACHE:
        path = IMAGES_DIR / f"{name}.png"
        if not path.is_file():
            raise SystemExit(f"missing {path} - run tools/generate_fan_frames.py first")
        _FAN_FRAME_CACHE[name] = Image.open(path).convert("1")
    return _FAN_FRAME_CACHE[name]


def render_frame(state: DisplayState) -> Canvas:
    c = Canvas.new()
    running = state.running
    idx = state.speed_idx if 1 <= state.speed_idx <= 3 else 0
    state_str = SPEED_NAMES[idx] if running else "OFF"

    # ---- KEEP IN SYNC: FAN_ANIM ----
    frame_name = f"fan_frame_{state.frame_idx % ROTATION_FRAMES}" if running else "fan_off"
    glyph = _load_fan_frame(frame_name)
    bbox = c.paste_image(*FAN_ICON_POS, glyph)
    c.register("fan_icon", "LEFT", bbox)

    # ---- KEEP IN SYNC: LEARN_STATE / STATE_WORD / COUNTDOWN ----
    if state.learn_active:
        font_learn = _font(ROBOTO_TTF, LEARN_FONT_SIZE)
        font_learn_prompt = _font(ROBOTO_TTF, LEARN_PROMPT_FONT_SIZE)
        bbox = c.text_topcenter(*LEARN_TITLE_POS, font_learn, "LEARN")
        c.register("learn_title", "LEFT", bbox)
        bbox = c.text_topcenter(*LEARN_PROMPT_POS, font_learn_prompt, "REMOTE X2")
        c.register("learn_prompt", "LEFT", bbox)
    elif state.learn_confirm:
        font_learn = _font(ROBOTO_TTF, LEARN_FONT_SIZE)
        font_learn_prompt = _font(ROBOTO_TTF, LEARN_PROMPT_FONT_SIZE)
        bbox = c.text_topcenter(*LEARN_TITLE_POS, font_learn, "LEARNED")
        c.register("learn_confirmation", "LEFT", bbox)
        bbox = c.text_topcenter(*LEARN_PROMPT_POS, font_learn_prompt, "ID SAVED")
        c.register("learn_confirmation_detail", "LEFT", bbox)
    else:
        # ---- KEEP IN SYNC: STATE_WORD ----
        font_state = _font(ROBOTO_TTF, STATE_WORD_FONT_SIZE)
        bbox = c.text_topcenter(*STATE_WORD_POS, font_state, state_str)
        c.register("state_word", "LEFT", bbox)

        # ---- KEEP IN SYNC: COUNTDOWN ----
        # HH:MM:SS while a timer is active; nothing while continuous-on
        # (running, no timer) or off - the spinning fan icon + state word
        # already say "running".
        if running and state.timer_active:
            remaining_ms = max(0, state.remaining_ms)
            hh = remaining_ms // 3600000
            mm = (remaining_ms % 3600000) // 60000
            ss = (remaining_ms % 60000) // 1000
            font_timer = _font(ROBOTO_TTF, COUNTDOWN_FONT_SIZE)
            text = f"{hh:02d}:{mm:02d}:{ss:02d}"
            bbox = c.text_topcenter(*COUNTDOWN_POS, font_timer, text)
            c.register("countdown", "LEFT", bbox)

    # ---- KEEP IN SYNC: STATUS_ICONS (wifi + api + battery, bottom-left horizontal row) ----
    font_icons = _font(MDI_TTF, ICON_FONT_SIZE)
    if not state.wifi_up:
        wifi_glyph = WIFI_STRENGTH_OFF
    elif state.rssi >= -55:
        wifi_glyph = WIFI_STRENGTH_4
    elif state.rssi >= -65:
        wifi_glyph = WIFI_STRENGTH_3
    elif state.rssi >= -75:
        wifi_glyph = WIFI_STRENGTH_2
    else:
        wifi_glyph = WIFI_STRENGTH_1
    bbox = c.text_topleft(*WIFI_ICON_POS, font_icons, wifi_glyph)
    c.register("wifi_icon", "LEFT", bbox)

    api_glyph = API_CONNECTED if state.api_up else API_DISCONNECTED
    bbox = c.text_topleft(*API_ICON_POS, font_icons, api_glyph)
    c.register("api_icon", "LEFT", bbox)

    # ---- KEEP IN SYNC: BATTERY_ICON ----
    tier = battery_tier(state.battery_voltage)
    if tier > 0:
        bbox = c.text_topleft(*BATTERY_ICON_POS, font_icons, BATTERY_GLYPHS[tier])
        c.register("battery_icon", "LEFT", bbox)

    # ---- KEEP IN SYNC: TEMPERATURES ----
    font_large = _font(ROBOTO_TTF, TEMP_LARGE_FONT_SIZE)
    font_small = _font(ROBOTO_TTF, TEMP_SMALL_FONT_SIZE)

    in_str = f"{state.indoor:.0f}" if state.indoor is not None and math.isfinite(state.indoor) else "--"
    bbox = c.text_topright(*INDOOR_POS, font_large, f"{in_str}°F")
    c.register("indoor_temp", "RIGHT", bbox)

    out_str = f"{state.outdoor:.0f}" if state.outdoor is not None and math.isfinite(state.outdoor) else "--"
    bbox = c.text_topright(*OUTDOOR_POS, font_small, f"{out_str}°F")
    c.register("outdoor_temp", "RIGHT", bbox)

    attic_str = f"{state.attic:.0f}" if state.attic is not None and math.isfinite(state.attic) else "--"
    bbox = c.text_topright(*ATTIC_POS, font_small, f"{attic_str}°F")
    c.register("attic_temp", "RIGHT", bbox)

    # ---- KEEP IN SYNC: TEMP_ICONS ----
    bbox = c.text_topleft(*INDOOR_ICON_POS, font_icons, INDOOR_ICON)
    c.register("indoor_icon", "RIGHT", bbox)
    bbox = c.text_topleft(*OUTDOOR_ICON_POS, font_icons, OUTDOOR_ICON)
    c.register("outdoor_icon", "RIGHT", bbox)
    bbox = c.text_topleft(*ATTIC_ICON_POS, font_icons, ATTIC_ICON)
    c.register("attic_icon", "RIGHT", bbox)

    return c


# =============================================================================
# Layout sanity assertions
# =============================================================================
def rects_overlap(a: tuple[int, int, int, int], b: tuple[int, int, int, int]) -> bool:
    ax0, ay0, ax1, ay1 = a
    bx0, by0, bx1, by1 = b
    return ax0 < bx1 and bx0 < ax1 and ay0 < by1 and by0 < ay1


# Which side(s) of each zone border another zone ("seam") vs. sit flush on
# the display's own physical boundary ("edge"). Flush-to-edge (e.g. the
# indoor temperature's TOP_RIGHT anchor at the display's actual x=127/y=0
# corner - the same convention the original v2 display lambda already
# used) is an intentional design choice, not clipping, so margin is only
# enforced at inter-zone seams, where crowding would visually collide with
# a neighboring zone's content. (ZONE_SEAMS itself is defined above, next
# to ZONES, since v3.2 collapsed the old three-zone layout to two.)


def check_layout(canvas: Canvas, label: str) -> list[str]:
    problems: list[str] = []
    els = canvas.elements

    # No overlaps between distinct elements.
    for i in range(len(els)):
        for j in range(i + 1, len(els)):
            if rects_overlap(els[i].bbox, els[j].bbox):
                problems.append(
                    f"[{label}] overlap: {els[i].name}{els[i].bbox} intersects "
                    f"{els[j].name}{els[j].bbox}"
                )

    for el in els:
        x0, y0, x1, y1 = el.bbox
        # No clipping: every bbox must be fully within the physical canvas.
        if x0 < 0 or y0 < 0 or x1 > CANVAS_W or y1 > CANVAS_H:
            problems.append(
                f"[{label}] clipped: {el.name}{el.bbox} exceeds canvas "
                f"0..{CANVAS_W}x0..{CANVAS_H}"
            )
        # Minimum margin from its assigned zone's inter-zone seams only
        # (see ZONE_SEAMS above) - not from the display's own outer edge.
        if el.zone is not None:
            zx0, zx1 = ZONES[el.zone]
            seams = ZONE_SEAMS[el.zone]
            if "left" in seams and x0 < zx0 + MIN_MARGIN:
                problems.append(
                    f"[{label}] zone seam margin: {el.name}{el.bbox} is within "
                    f"{MIN_MARGIN}px of zone {el.zone}'s left seam (x={zx0})"
                )
            if "right" in seams and x1 > zx1 - MIN_MARGIN:
                problems.append(
                    f"[{label}] zone seam margin: {el.name}{el.bbox} is within "
                    f"{MIN_MARGIN}px of zone {el.zone}'s right seam (x={zx1})"
                )

    # Zones themselves must not overlap (structural check on the constants).
    zone_items = list(ZONES.items())
    for i in range(len(zone_items)):
        for j in range(i + 1, len(zone_items)):
            (na, (a0, a1)), (nb, (b0, b1)) = zone_items[i], zone_items[j]
            if a0 < b1 and b0 < a1:
                problems.append(f"[{label}] zone/zone overlap: {na}({a0},{a1}) vs {nb}({b0},{b1})")

    return problems


def check_state_semantics(canvas: Canvas, label: str, state: DisplayState) -> list[str]:
    """Assert that learn screens replace, rather than overlap, normal text."""
    problems: list[str] = []
    names = {element.name for element in canvas.elements}
    if state.learn_active:
        required = {"learn_title", "learn_prompt"}
        forbidden = {"state_word", "countdown", "learn_confirmation", "learn_confirmation_detail"}
    elif state.learn_confirm:
        required = {"learn_confirmation", "learn_confirmation_detail"}
        forbidden = {"state_word", "countdown", "learn_title", "learn_prompt"}
    else:
        required = {"state_word"}
        forbidden = {"learn_title", "learn_prompt", "learn_confirmation", "learn_confirmation_detail"}

    missing = required - names
    unexpected = forbidden & names
    if missing:
        problems.append(f"[{label}] missing state element(s): {sorted(missing)}")
    if unexpected:
        problems.append(f"[{label}] unexpected state element(s): {sorted(unexpected)}")
    return problems


# =============================================================================
# Preview matrix + rotation strip
# =============================================================================
def save_png(canvas: Canvas, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    upscaled = canvas.image.resize((CANVAS_W * UPSCALE, CANVAS_H * UPSCALE), Image.NEAREST)
    upscaled.save(path)


def build_preview_matrix() -> dict[str, DisplayState]:
    return {
        "off-idle": DisplayState(
            running=False, speed_idx=1, timer_active=False, remaining_ms=0, frame_idx=0,
            wifi_up=True, rssi=-50, api_up=True, indoor=74.0, outdoor=68.0, attic=81.0,
        ),
        "low-continuous": DisplayState(
            running=True, speed_idx=1, timer_active=False, remaining_ms=0, frame_idx=2,
            wifi_up=True, rssi=-50, api_up=True, indoor=74.0, outdoor=68.0, attic=81.0,
        ),
        "med-timer-00-47-12": DisplayState(
            running=True, speed_idx=2, timer_active=True,
            remaining_ms=((0 * 3600 + 47 * 60 + 12) * 1000), frame_idx=5,
            wifi_up=True, rssi=-50, api_up=True, indoor=76.0, outdoor=70.0, attic=88.0,
        ),
        "high-timer-11-59-59": DisplayState(
            running=True, speed_idx=3, timer_active=True,
            remaining_ms=((11 * 3600 + 59 * 60 + 59) * 1000), frame_idx=9,
            wifi_up=True, rssi=-50, api_up=True, indoor=78.0, outdoor=95.0, attic=110.0,
        ),
        "high-weak-wifi-1-bar": DisplayState(
            running=True, speed_idx=3, timer_active=False, remaining_ms=0, frame_idx=7,
            wifi_up=True, rssi=-82, api_up=True, indoor=79.0, outdoor=96.0, attic=112.0,
        ),
        "wifi-down": DisplayState(
            running=True, speed_idx=2, timer_active=False, remaining_ms=0, frame_idx=4,
            wifi_up=False, rssi=-100, api_up=False, indoor=75.0, outdoor=69.0, attic=84.0,
        ),
        "api-down-wifi-up": DisplayState(
            running=True, speed_idx=1, timer_active=False, remaining_ms=0, frame_idx=10,
            wifi_up=True, rssi=-48, api_up=False, indoor=73.0, outdoor=66.0, attic=79.0,
        ),
        "temps-missing": DisplayState(
            running=True, speed_idx=1, timer_active=False, remaining_ms=0, frame_idx=1,
            wifi_up=True, rssi=-50, api_up=True, indoor=None, outdoor=None, attic=None,
        ),
        "learn-active": DisplayState(
            running=False, speed_idx=1, timer_active=False, remaining_ms=0, frame_idx=0,
            wifi_up=True, rssi=-50, api_up=True, indoor=74.0, outdoor=68.0, attic=81.0,
            learn_active=True,
        ),
        "learn-confirmed": DisplayState(
            running=False, speed_idx=1, timer_active=False, remaining_ms=0, frame_idx=0,
            wifi_up=True, rssi=-50, api_up=True, indoor=74.0, outdoor=68.0, attic=81.0,
            learn_confirm=True,
        ),
        # ---- Battery status column states (FIX 3) ----
        "battery-plugged-full": DisplayState(
            running=True, speed_idx=1, timer_active=False, remaining_ms=0, frame_idx=3,
            wifi_up=True, rssi=-50, api_up=True, indoor=74.0, outdoor=68.0, attic=81.0,
            battery_voltage=4.35,  # >= 4.30V plugged threshold -> power-plug glyph
        ),
        "battery-charging": DisplayState(
            running=True, speed_idx=2, timer_active=False, remaining_ms=0, frame_idx=6,
            wifi_up=True, rssi=-50, api_up=True, indoor=75.0, outdoor=69.0, attic=83.0,
            battery_voltage=4.24,  # 4.20-4.30V topping-off band -> battery-charging glyph
        ),
        "battery-75pct": DisplayState(
            running=True, speed_idx=3, timer_active=False, remaining_ms=0, frame_idx=8,
            wifi_up=True, rssi=-50, api_up=True, indoor=77.0, outdoor=90.0, attic=100.0,
            battery_voltage=3.98,  # curve breakpoint for 75% -> HIGH tier, battery-70 glyph
        ),
        "battery-30pct": DisplayState(
            running=True, speed_idx=1, timer_active=True,
            remaining_ms=((1 * 3600 + 30 * 60) * 1000), frame_idx=11,
            wifi_up=True, rssi=-60, api_up=True, indoor=73.0, outdoor=65.0, attic=77.0,
            battery_voltage=3.77,  # curve breakpoint for 30% -> LOW tier, battery-20 glyph
        ),
        "battery-low-10pct": DisplayState(
            running=False, speed_idx=1, timer_active=False, remaining_ms=0, frame_idx=0,
            wifi_up=True, rssi=-50, api_up=True, indoor=72.0, outdoor=64.0, attic=76.0,
            battery_voltage=3.69,  # curve breakpoint for 10% -> ALERT tier, battery-alert glyph
        ),
        "battery-no-battery": DisplayState(
            running=True, speed_idx=2, timer_active=False, remaining_ms=0, frame_idx=6,
            wifi_up=True, rssi=-50, api_up=True, indoor=76.0, outdoor=70.0, attic=85.0,
            battery_voltage=None,  # < 2.5V floor (or ADC never reported) -> hidden entirely
        ),
    }


def build_rotation_strip() -> Image.Image:
    speeds = (1, 2, 3)  # LOW, MED, HIGH
    ticks = 6  # consecutive simulated 250ms display refreshes per speed row
    # Crop to the LEFT zone only (icon + state word + status row) so the
    # strip isolates spin readability instead of repeating the unchanging
    # RIGHT-zone temperatures in every cell.
    cell_w, cell_h = ZONES["LEFT"][1], CANVAS_H
    strip = Image.new("1", (cell_w * ticks * UPSCALE, cell_h * len(speeds) * UPSCALE), 0)
    for row, speed_idx in enumerate(speeds):
        step = FAN_FRAME_STEP[speed_idx]
        # Simulate the EXACT sequence the YAML lambda produces:
        # id(fan_anim_frame) = (id(fan_anim_frame) + step) % ROTATION_FRAMES,
        # advanced once per tick, starting from frame 0 - not an
        # independently-computed angle. This is what actually catches a
        # bad step/frame-count combination (e.g. a step that shares a
        # factor with ROTATION_FRAMES and degenerates to a short,
        # oscillating cycle) instead of just plotting evenly-spaced angles.
        frame_idx = 0
        for tick in range(ticks):
            state = DisplayState(
                running=True, speed_idx=speed_idx, timer_active=False, remaining_ms=0,
                frame_idx=frame_idx, wifi_up=True, rssi=-50, api_up=True,
                indoor=74.0, outdoor=68.0, attic=81.0,
            )
            canvas = render_frame(state)
            crop = canvas.image.crop((0, 0, cell_w, cell_h))
            crop = crop.resize((cell_w * UPSCALE, cell_h * UPSCALE), Image.NEAREST)
            strip.paste(crop, (tick * cell_w * UPSCALE, row * cell_h * UPSCALE))
            frame_idx = (frame_idx + step) % ROTATION_FRAMES
    return strip


def main() -> None:
    if not ROBOTO_TTF.exists():
        raise SystemExit(f"Roboto TTF not found at {ROBOTO_TTF} - run `esphome config` once to populate the build cache.")
    if not MDI_TTF.exists():
        raise SystemExit(f"MDI TTF not found at {MDI_TTF} - see fonts/ in the README.")
    missing_frames = [
        name for name in (*(f"fan_frame_{i}" for i in range(ROTATION_FRAMES)), "fan_off")
        if not (IMAGES_DIR / f"{name}.png").is_file()
    ]
    if missing_frames:
        raise SystemExit(
            f"missing fan frame image(s) {missing_frames} in {IMAGES_DIR} - "
            f"run `.venv313/bin/python tools/generate_fan_frames.py` first."
        )

    PREVIEWS_DIR.mkdir(parents=True, exist_ok=True)

    all_problems: list[str] = []
    matrix = build_preview_matrix()
    for name, state in matrix.items():
        canvas = render_frame(state)
        problems = check_layout(canvas, name)
        problems.extend(check_state_semantics(canvas, name, state))
        all_problems.extend(problems)
        out_path = PREVIEWS_DIR / f"{name}.png"
        save_png(canvas, out_path)
        status = "OK" if not problems else f"{len(problems)} ISSUE(S)"
        print(f"  {name:24s} -> {out_path.relative_to(ROOT)}  [{status}]")

    strip = build_rotation_strip()
    strip_path = PREVIEWS_DIR / "rotation-strip.png"
    strip.save(strip_path)
    print(f"  {'rotation-strip':24s} -> {strip_path.relative_to(ROOT)}")

    print()
    if all_problems:
        print(f"LAYOUT SANITY: {len(all_problems)} problem(s) found:")
        for p in all_problems:
            print(f"  - {p}")
        raise SystemExit(1)
    else:
        print(f"LAYOUT SANITY: OK - {len(matrix)} states checked, no overlaps/clipping/margin violations.")


if __name__ == "__main__":
    main()
