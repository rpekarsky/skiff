#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""The UI palettes (run by tools/build.py generate()).

  gen_ui_palettes.py OUT.h [--report]

A palette is five colours: BG (background), SURF (surface: dialogs, menu rows, mixer strips),
TEXT, THEME (identity colour: values, curves, gauges, selection) and ACCENT (the one active
thing: hot knob, cursor, playhead, a sounding step). The firmware derives the rest with fixed
blends at palette_set() time (src/gfx.c, the same integer maths as mix() here):
  MID  = mix(BG, TEXT, 70 %)    labels, units, secondary text
  DIM  = mix(BG, TEXT, 42 %)    inactive, empty steps, disabled
  LINE = mix(BG, TEXT, 18 %)    1 px dividers, the faint gauge track
  SEL  = mix(BG, THEME, 80 %)   selection fill and gauge fill (the "mid" of the theme)
  TINT = mix(BG, THEME, 12 %)   a faint area (the selected drum lane)
  RAISE = mix(SURF, TEXT, 12 %) a raised area on a surface: empty step stubs, guides, the selected drum
                                lane, fader slots, button wells, chips
  KEY  = mix(BG, TEXT, 78 %)    a keycap's fill (a mid-light cushion; its label INK, unavailable: a DIM fill)
  INK  = BG: text on a selection or THEME fill
Outside the palette: REC, a fixed red; the QR code's fixed black and white; the crash screen's fixed red.
(Skiff, the review's R1) The palettes are the MENU's THEME: SKIFF (the default), HI-CON and PAPER (for reading in
bright or dim light), CRYPT and NEON (their own font and widgets besides: src/gfx.c ux.crypt, ux.neon), QUARK (its own
widgets: ux.quark), (#29) LCD and LCD NIGHT (four tones, CRYPT's font, their own widgets: ux.h1n), then the plain
GAMEBOY, AMBER and BLUE-STN (wishlist #47: old handhelds' and synths' LCDs), then LCD GRAIN (LCD's look with the cells'
gaps instead of the dot grid) and LCD GREEN (LCD's look in its first, reflective green-grey). Saved settings name them by a tagged id
(THEME_TAG + index). Earlier ids are migrated: PAL_TAG + an index into the eleven palettes before THEME
(UI_PALETTE_MIGRATE11), and below 20 Felucca's oldest twenty (UI_PALETTE_MIGRATE20); a palette that is gone is SKIFF.
--report prints the WCAG contrast of every pairing. The tool fails (exit 1) on any miss.
"""
import argparse
import sys
from pathlib import Path

# name, bg, surf, text, theme, accent (8-bit RGB; stored as RGB565)
PALETTES = [
    # Skiff's BLOCKS look (DESIGN.md): neutral tiles, white values, the one active thing inverted white; the colour
    # is the tracks' (TRACKS)
    ("SKIFF",  (12, 12, 12),    (30, 30, 30),    (255, 255, 255), (255, 255, 255), (255, 255, 255)),
    ("HI-CON", (0, 0, 0),       (40, 40, 40),    (255, 255, 255), (255, 232, 0),   (0, 230, 255)),
    ("PAPER",  (244, 239, 228), (226, 218, 202), (34, 30, 24),    (10, 84, 70),    (172, 56, 8)),
    # (Skiff) CRYPT, design/mock_crypt.py's B TORCHLIT: stone on the dark, bone text, amber values, flame for the one
    # active thing; the tracks ember, gold, bone, blood, olive, mauve
    ("CRYPT",  (10, 7, 5),      (34, 24, 16),    (236, 214, 172), (255, 176, 32),  (255, 222, 120)),
    # (Skiff) NEON, design/mock_neon.py's A SUNSET: a purple night, glass, pink neon for values, cyan for the one
    # active thing; the tracks pink, cyan, yellow, orange, violet, mint
    ("NEON",   (12, 4, 26),     (30, 12, 50),    (244, 236, 255), (255, 88, 190),  (48, 224, 255)),
    # (Skiff) QUARK, design/mock_quark.py's A COLLIDER: a detector's dark, cyan readouts, gold for the one active thing;
    # the tracks the colour charges red, green, blue and their anticolours cyan, magenta, yellow
    ("QUARK",  (3, 7, 12),      (10, 21, 32),    (216, 243, 255), (79, 209, 255),  (255, 224, 102)),
    # (Skiff #29) LCD, the Zoom H1n's backlit display (PO, after a photo of the real one: cool blue-white, not the H1N
    # GREY mockup): four tones only, BG, the unlit segments' ghost and MID (LCD_OWN_TONES), navy ink. No tiles (SURF is
    # BG, as an LCD has no boxes); src/gfx.c lcd_tones pins the derived tokens to the four; every track ink, told apart
    # by its number
    ("LCD",    (214, 228, 246), (214, 228, 246), (18, 30, 84),    (18, 30, 84),    (18, 30, 84)),
    # (Skiff #29) LCD NIGHT, the PO's pick of THEME CUSTOM over LCD (tests/ui_render.c's dusk set), as it shows: an
    # inverted LCD, navy, pale ink. SURF stays the dusk's, as it makes the ghost (RAISE); THEME is the MID tone, which
    # the dusk's orange (and five of its six tracks) snapped to; ACCENT the ink
    ("LCD NIGHT", (16, 24, 32), (28, 40, 54),   (242, 232, 216), (156, 149, 148), (242, 232, 216)),
    # (wishlist #47) the LCD mockups (bg, off-segment ghost, mid, ink), lightened or darkened until they pass the
    # CHECKS: the fixed REC needs a light BG near PAPER's luminance, a dark one near black. A monochrome LCD has no
    # second hue, so its ACCENT is its ink, as SKIFF's is its white; the tracks are THEME, as PAPER's
    ("GAMEBOY", (196, 232, 72), (178, 216, 64),  (9, 32, 9),       (36, 74, 36),    (9, 32, 9)),
    ("AMBER",  (255, 212, 100), (240, 194, 84),  (40, 20, 4),      (92, 54, 8),     (40, 20, 4)),
    ("BLUE-STN", (22, 32, 92),  (31, 45, 115),   (232, 238, 255), (150, 170, 240), (255, 255, 255)),
    # (PO, 31.10) LCD GRAIN: LCD's colours and look, the dot grid replaced by the gaps between the LCD's cells, the
    # backlight showing through every third row and column (src/gfx.c cv_lcd_gaps); after the others: ids stay
    ("LCD GRAIN", (214, 228, 246), (214, 228, 246), (18, 30, 84), (18, 30, 84),  (18, 30, 84)),
    # (wishlist #66) LCD GREEN: LCD as it was before the H1n's backlit blue, the PO's H1N GREY (a reflective LCD): the
    # ghost and MID derived (RAISE 160,170,148; BG to ink LCD_MID_PCT)
    ("LCD GREEN", (178, 186, 166), (178, 186, 166), (28, 32, 26),  (28, 32, 26),  (28, 32, 26)),
    # (PO, 02.11, a test) LCD GRAIN's cell gaps at 25 % and 75 % (LCD GRAIN: 50 %), to compare on the unit
    ("GRAIN 25", (214, 228, 246), (214, 228, 246), (18, 30, 84),   (18, 30, 84),  (18, 30, 84)),
    ("GRAIN 75", (214, 228, 246), (214, 228, 246), (18, 30, 84),   (18, 30, 84),  (18, 30, 84)),
]
SKIFF, CRYPT, NEON, QUARK, LCD, LCD_NIGHT = "SKIFF", "CRYPT", "NEON", "QUARK", "LCD", "LCD NIGHT"
LCD_GRAIN, LCD_GREEN, GRAIN_25, GRAIN_75 = "LCD GRAIN", "LCD GREEN", "GRAIN 25", "GRAIN 75"
LCD_LOOKS = (LCD, LCD_NIGHT, LCD_GRAIN, LCD_GREEN, GRAIN_25, GRAIN_75)   # src/gfx.c ux.h1n: four tones, CRYPT's font, ui_h1n.c's widgets
# LCD's ghost and MID as the backlit H1n shows them: blue, off the line from BG to ink that the derived ones lie on
# (src/gfx.c lcd_tones; THEME CUSTOM over the look derives its own)
LCD_OWN_TONES = {LCD: ((150, 190, 240), (96, 128, 196)), LCD_GRAIN: ((150, 190, 240), (96, 128, 196)),
                 GRAIN_25: ((150, 190, 240), (96, 128, 196)), GRAIN_75: ((150, 190, 240), (96, 128, 196))}
# the six tracks' colours (src/gfx.c T_TRK; (Skiff phase 6) T5, T6 too): a palette not named here gives every track
# its THEME. Told apart by lightness as well as hue, and always drawn with the track's number (never the colour alone)
TRACKS = {"SKIFF": [(255, 90, 54), (34, 195, 180), (255, 191, 0), (149, 128, 255),    # vermilion teal amber violet
                    (255, 111, 174), (163, 230, 53)],                                 # pink lime
          "CRYPT": [(236, 110, 50), (242, 182, 60), (222, 206, 166), (232, 88, 66),   # ember gold bone blood
                    (172, 172, 92), (204, 134, 176)],                                 # olive mauve
          "NEON": [(255, 64, 176), (48, 224, 255), (255, 222, 90), (255, 128, 56),    # pink cyan yellow orange
                   (170, 100, 255), (80, 255, 170)],                                  # violet mint
          "QUARK": [(255, 84, 96), (70, 220, 130), (90, 140, 255), (60, 220, 240),    # red green blue, anti: cyan
                    (230, 96, 230), (250, 220, 80)]}                                  # magenta yellow
NTRACKS = 6
# the eleven palettes before THEME (index order: their ids PAL_TAG + index) and Felucca's oldest twenty (ids below 20)
OLD11 = ["GREY", "GREEN", "AMBER", "ICE", "VIOLET", "ROSE", "PAPER", "HI-CON", "NIGHT", "MONO", "SKIFF"]
OLD = ["GREEN", "AMBER", "CYAN", "RED", "MONO", "VIOLET", "PINK", "ICE", "WARM", "OCEAN", "DUSK", "HI-CON",
       "LIGHT", "PAPER", "SKY", "MINT", "LILAC", "ROSE", "SAND", "L-HICON"]
OLD_TO_NEW = {"HI-CON": "HI-CON", "L-HICON": "HI-CON", "PAPER": "PAPER", "LIGHT": "PAPER", "SKY": "PAPER",
              "MINT": "PAPER", "LILAC": "PAPER", "SAND": "PAPER", "ROSE": "PAPER"}   # (the twenty's ROSE was light)
OLD11_TO_NEW = {"HI-CON": "HI-CON", "PAPER": "PAPER", "SKIFF": "SKIFF"}           # the rest: SKIFF
PAL_TAG = 64                       # (before THEME) stored id = PAL_TAG + an index into OLD11
THEME_TAG = 128                    # stored id = THEME_TAG + index
REC_DARK, REC_LIGHT = (255, 72, 72), (190, 24, 40)
QR_LIGHT, QR_DARK, CRASH_BG, CRASH_INK = (255, 255, 255), (0, 0, 0), (160, 0, 0), (255, 255, 255)
PCT = {"MID": 70, "DIM": 42, "LINE": 18, "SEL": 80, "TINT": 12, "KEY": 78}
RAISE_PCT = 12                     # SURF -> TEXT
LCD_MID_PCT = 60                   # (LCD) its MID tone: BG -> TEXT


def to565(c, exact=False):
    r, g, b = c
    if r == g == b and not exact:                     # a gray stays on the RGB565 gray axis (G = 2 R)
        return ((r >> 3) << 11) | ((r >> 3) << 6) | (r >> 3)
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def from565(v):
    return ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def cdiv(a, b):
    return a // b if a >= 0 else -((-a) // b)


def mix(a, b, pct):
    """src/gfx.c ux_mix: a + (b - a) * pct / 100 per channel, rounded"""
    out = 0
    for sh, mask in ((11, 31), (5, 63), (0, 31)):
        x, y = (a >> sh) & mask, (b >> sh) & mask
        d = (y - x) * pct
        out |= (x + cdiv(d + (50 if d >= 0 else -50), 100)) << sh
    return out


def luma(c):
    """src/gfx.c ux_luma: 0..255, integer"""
    r, g, b = c >> 11, (c >> 5) & 63, c & 31
    return (r * 255 // 31 * 54 + g * 255 // 63 * 183 + b * 255 // 31 * 19) >> 8


def lum(c565):
    def lin(v):
        v /= 255.0
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = from565(c565)
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def contrast(a, b):
    la, lb = lum(a), lum(b)
    return (max(la, lb) + 0.05) / (min(la, lb) + 0.05)


def derive(p):
    name, *cols = p
    bg, surf, text, theme, accent = (to565(c) for c in cols)
    d = dict(bg=bg, surf=surf, text=text, theme=theme, accent=accent,
             mid=mix(bg, text, PCT["MID"]), dim=mix(bg, text, PCT["DIM"]),
             line=mix(bg, text, PCT["LINE"]), sel=mix(bg, theme, PCT["SEL"]),
             tint=mix(bg, theme, PCT["TINT"]), raise_=mix(surf, text, RAISE_PCT),
             key=mix(bg, text, PCT["KEY"]))
    d["ink"] = bg
    d["light"] = luma(bg) > 128
    d["rec"] = to565(REC_LIGHT if d["light"] else REC_DARK)
    if name in LCD_LOOKS:                             # src/gfx.c lcd_tones
        mid = mix(bg, text, LCD_MID_PCT)
        if name in LCD_OWN_TONES:
            d["raise_"], mid = (to565(c) for c in LCD_OWN_TONES[name])
        d.update(mid=mid, dim=mid, line=d["raise_"], tint=d["raise_"], sel=text, key=text, rec=text)
    return name, d


# (foreground, background, minimum contrast, why)
CHECKS = [("text", "bg", 12.0, "body text"), ("text", "surf", 9.0, "body text on a surface"),
          ("mid", "bg", 5.0, "labels"), ("mid", "surf", 4.0, "labels on a surface"),
          ("dim", "bg", 2.2, "inactive (not read)"), ("line", "bg", 1.25, "1 px dividers"),
          ("theme", "bg", 4.5, "values, curves"), ("theme", "surf", 4.5, "values on a surface"),
          ("accent", "bg", 4.5, "hot value, cursor, playhead"), ("accent", "surf", 4.5, "hot on a surface"),
          ("ink", "sel", 4.5, "text on a selection"), ("ink", "theme", 4.5, "text on a THEME fill"),
          ("sel", "bg", 1.8, "gauge fill vs track"), ("rec", "bg", 3.0, "REC (graphic)"),
          ("text", "raise_", 7.0, "text on a RAISE chip or button"), ("ink", "rec", 4.5, "text on the REC chip"),
          ("ink", "key", 4.5, "a keycap's label"), ("key", "bg", 3.0, "a keycap on the background"),
          ("key", "surf", 2.5, "a keycap on a surface"), ("ink", "dim", 1.8, "an unavailable keycap's label"),
          ("ink", "accent", 4.5, "text on the ARM badge")]
# (LCD) a reflective LCD's floor: its ink 8.3:1 on its BG at best, the unlit ghost 1.2:1; the checks it can't meet
# with four tones, at what its tones give (rounded down)
LCD_MIN = {("text", "bg"): 8.0, ("text", "surf"): 8.0, ("mid", "bg"): 3.0, ("mid", "surf"): 3.0, ("line", "bg"): 1.2,
           ("text", "raise_"): 6.5}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", nargs="?")
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()
    fails = []
    assert PALETTES[0][0] == SKIFF and len(OLD) == 20 and len(OLD11) == 11 and len(PALETTES) <= 100
    rows = [derive(p) for p in PALETTES]
    for name, d in rows:
        for f, b, lim, why in CHECKS:
            lim = LCD_MIN.get((f, b), lim) if name in LCD_LOOKS else lim
            c = contrast(d[f], d[b])
            if c < lim:
                fails.append(f"{name}: {f}/{b} {c:.2f} < {lim} ({why})")
        for k, t in enumerate(TRACKS.get(name, [])):   # a track colour: a lane on the background, its number on it
            t = to565(t)
            for c, lim, why in ((contrast(t, d["bg"]), 3.0, "track colour on BG"),
                                (contrast(t, d["surf"]), 3.0, "track colour on SURF"),
                                (contrast(d["ink"], t), 4.5, "the track's number on its colour")):
                if c < lim:
                    fails.append(f"{name}: track {k + 1} {c:.2f} < {lim} ({why})")
    if a.report:
        print(f"{'palette':8s} " + " ".join(f"{f}/{b}".ljust(12) for f, b, _, _ in CHECKS))
        for name, d in rows:
            print(f"{name:8s} " + " ".join(f"{contrast(d[f], d[b]):5.1f}".ljust(12) for f, b, _, _ in CHECKS))
        print("minimum  " + " ".join(f"{lim:<12.2f}" for _, _, lim, _ in CHECKS))
    if a.out:
        names = [p[0] for p in PALETTES]
        out = ["/* generated by tools/gen_ui_palettes.py: the UI palettes (5 colours each, the rest derived) */",
               "#pragma once", "#include <stdint.h>", "",
               "/* ui_pal_t {name, bg, surf, text, theme, accent} (src/gfx.c) */",
               "static const ui_pal_t UI_PALETTES[] = {"]
        for p in PALETTES:
            v = [to565(c) for c in p[1:]]
            out.append(f'    {{"{p[0]}", ' + ", ".join(f"0x{x:04x}" for x in v) + "},")
        out += ["};", "/* the six tracks' colours per palette (src/gfx.c T_TRK; a palette without its own: THEME) */",
                f"#define UI_NTRK {NTRACKS}u", f"static const uint16_t UI_TRACKS[][UI_NTRK] = {{"]
        for p in PALETTES:
            t = [to565(c) for c in TRACKS[p[0]]] if p[0] in TRACKS else [to565(p[4])] * NTRACKS
            out.append("    {" + ", ".join(f"0x{x:04x}" for x in t) + f"}},   /* {p[0]} */")
        out += ["};", f"#define UI_NPALETTES {len(PALETTES)}u",
                f"#define UI_SKIFF_INDEX {names.index(SKIFF)}u          /* Skiff's default (src/panel.c settings_init) */",
                f"#define UI_CRYPT_INDEX {names.index(CRYPT)}u          /* CRYPT: its own font and widgets too */",
                f"#define UI_NEON_INDEX {names.index(NEON)}u          /* NEON: CRYPT's font, its own widgets */",
                f"#define UI_QUARK_INDEX {names.index(QUARK)}u          /* QUARK: Skiff's font, its own widgets */",
                f"#define UI_LCD_INDEX {names.index(LCD)}u          /* LCD: four tones, CRYPT's font, its own widgets */",
                f"#define UI_LCD_NIGHT_INDEX {names.index(LCD_NIGHT)}u          /* LCD NIGHT: LCD's look, its own colours */",
                f"#define UI_LCD_GRAIN_INDEX {names.index(LCD_GRAIN)}u          /* LCD GRAIN: LCD, the cells' gaps for the dots */",
                f"#define UI_LCD_GREEN_INDEX {names.index(LCD_GREEN)}u          /* LCD GREEN: LCD in its first green-grey */",
                f"#define UI_GRAIN_25_INDEX {names.index(GRAIN_25)}u          /* GRAIN 25: LCD GRAIN, its gaps at 25 % */",
                f"#define UI_GRAIN_75_INDEX {names.index(GRAIN_75)}u          /* GRAIN 75: .. at 75 % */",
                f"#define UI_THEME_TAG {THEME_TAG}u          /* stored id = UI_THEME_TAG + index */",
                f"#define UI_PAL_TAG {PAL_TAG}u          /* before THEME: UI_PAL_TAG + an index into the eleven; below 20: "
                "Felucca's oldest */"]
        out += [f"#define UI_{k}_PCT {v}" for k, v in PCT.items()] + [f"#define UI_RAISE_PCT {RAISE_PCT}   /* SURF -> TEXT */",
                                                    f"#define UI_LCD_MID_PCT {LCD_MID_PCT}   /* (LCD) BG -> TEXT */"]
        ghost, mid = (to565(c) for c in LCD_OWN_TONES[LCD])
        out += [f"#define UI_LCD_GHOST 0x{ghost:04x}u   /* (LCD) its own ghost and MID (CUSTOM over it: derived) */",
                f"#define UI_LCD_MID 0x{mid:04x}u"]
        out += [f"#define UI_REC_DARK 0x{to565(REC_DARK):04x}u", f"#define UI_REC_LIGHT 0x{to565(REC_LIGHT):04x}u",
                f"#define UI_QR_LIGHT 0x{to565(QR_LIGHT):04x}u", f"#define UI_QR_DARK 0x{to565(QR_DARK):04x}u",
                f"#define UI_CRASH_BG 0x{to565(CRASH_BG):04x}u", f"#define UI_CRASH_INK 0x{to565(CRASH_INK):04x}u", "",
                "/* the eleven palettes before THEME: " + " ".join(OLD11) + " */",
                "static const uint8_t UI_PALETTE_MIGRATE11[11] = {" +
                ", ".join(str(names.index(OLD11_TO_NEW.get(n, SKIFF))) for n in OLD11) + "};",
                "/* Felucca's oldest twenty: " + " ".join(OLD) + " */",
                "static const uint8_t UI_PALETTE_MIGRATE20[20] = {" +
                ", ".join(str(names.index(OLD_TO_NEW.get(n, SKIFF))) for n in OLD) + "};", ""]
        Path(a.out).write_text("\n".join(out))
    for f in fails:
        print("FAIL", f)
    if fails:
        sys.exit(1)


if __name__ == "__main__":
    main()
