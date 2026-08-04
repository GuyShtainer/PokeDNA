#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""gen_font.py — emit source/ui_font.c, PokeDNA's own proportional 5x7 font.

WHY THIS EXISTS
---------------
tonc's `sys8` is a FIXED 8 px font, and its glyphs already ink 6 of those 8
columns, so nothing proportional can be squeezed out of it. Every list in the
tool therefore truncated: `SUPER PO~`, `JIGGLYPU~`, `CHES~`.

The retail games do not have that problem because their font is variable-width
(~5 px per capital). Emerald's own bag prints `SUPER POTION` and
`No01 FOCUS PUNCH` in the same pixel budget where we managed nine characters.

So this file draws a 5x7 proportional face of our own. Every glyph below was
authored here as ASCII art -- it is NOT extracted, traced or converted from any
game ROM or decomp (see docs/kb/licensing.md and hard rule 6). The shapes are the
generic pixel-grid forms that any 5x7 face converges on; nothing is copied.

OUTPUT (git-ignored, generate locally):
    source/ui_font.c   glyph bitmaps + per-glyph advance table

    python3 tools/gen_font.py            # write the C file
    python3 tools/gen_font.py --proof out.png   # render a proof sheet and look at it

STORAGE FORMAT (matches what ui.c's blitter expects)
    96 glyphs, ASCII 32..127, 8 bytes each (one byte per row, top row first).
    Bit 0 of a row byte is the LEFTMOST pixel -- the same order tonc's sys8 uses,
    so the two fonts can share a blitter if that is ever wanted.
    s_w[i] is the ADVANCE in pixels (ink width + the 1 px inter-glyph gap).
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "source", "ui_font.c")

# Rows 0..6 are the body; row 7 exists for descenders (g j p q y ,).
# Baseline is row 6. Lowercase x-height is rows 2..6.
G = {}


def g(ch, *rows):
    """Register a glyph. Rows are '#'/'.' art; width = the widest row."""
    assert len(rows) <= 8, ch
    G[ch] = list(rows) + ["" for _ in range(8 - len(rows))]


# ---------------------------------------------------------------- punctuation
g(" ", "", "", "", "", "", "", "", "")
g("!", "#", "#", "#", "#", "#", ".", "#", "")
g('"', "#.#", "#.#", "", "", "", "", "", "")
g("#", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", "", "", "")
g("$", "..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#..", "")
g("%", "##..#", "##..#", "...#.", "..#..", ".#...", "#..##", "#..##", "")
g("&", ".##..", "#..#.", ".##..", "#..#.", "#...#", "#..#.", ".##.#", "")
g("'", "#", "#", "", "", "", "", "", "")
g("(", "..#", ".#.", "#..", "#..", "#..", ".#.", "..#", "")
g(")", "#..", ".#.", "..#", "..#", "..#", ".#.", "#..", "")
g("*", ".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "", "")
g("+", "", "..#..", "..#..", "#####", "..#..", "..#..", "", "")
g(",", "", "", "", "", "", ".#", ".#", "#.")
g("-", "", "", "", "#####", "", "", "", "")
g(".", "", "", "", "", "", "", "#", "")
g("/", "....#", "...#.", "..#..", "..#..", ".#...", "#....", "#....", "")
g(":", "", "", "#", "", "", "#", "", "")
g(";", "", "", ".#", "", "", ".#", ".#", "#.")
g("<", "...#", "..#.", ".#..", "#...", ".#..", "..#.", "...#", "")
g("=", "", "", "#####", "", "#####", "", "", "")
g(">", "#...", ".#..", "..#.", "...#", "..#.", ".#..", "#...", "")
g("?", ".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#..", "")
g("@", ".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".####", "")
g("[", "##", "#.", "#.", "#.", "#.", "#.", "##", "")
g("\\", "#....", "#....", ".#...", "..#..", "..#..", "...#.", "....#", "")
g("]", "##", ".#", ".#", ".#", ".#", ".#", "##", "")
g("^", "..#..", ".#.#.", "#...#", "", "", "", "", "")
g("_", "", "", "", "", "", "", "", "#####")
g("`", "#.", ".#", "", "", "", "", "", "")
g("{", "..##", ".#..", ".#..", "##..", ".#..", ".#..", "..##", "")
g("|", "#", "#", "#", "#", "#", "#", "#", "")
g("}", "##..", "..#.", "..#.", "..##", "..#.", "..#.", "##..", "")
g("~", "", "", ".#..#", "#.##.", "", "", "", "")

# --------------------------------------------------------------------- digits
g("0", ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###.", "")
g("1", "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###.", "")
g("2", ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####", "")
g("3", "####.", "....#", "....#", ".###.", "....#", "....#", "####.", "")
g("4", "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#.", "")
g("5", "#####", "#....", "####.", "....#", "....#", "#...#", ".###.", "")
g("6", "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###.", "")
g("7", "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#...", "")
g("8", ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###.", "")
g("9", ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##..", "")

# ------------------------------------------------------------------ uppercase
g("A", ".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#", "")
g("B", "####.", "#...#", "#...#", "####.", "#...#", "#...#", "####.", "")
g("C", ".###.", "#...#", "#....", "#....", "#....", "#...#", ".###.", "")
g("D", "###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###..", "")
g("E", "#####", "#....", "#....", "####.", "#....", "#....", "#####", "")
g("F", "#####", "#....", "#....", "####.", "#....", "#....", "#....", "")
g("G", ".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####", "")
g("H", "#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#", "")
g("I", "###", ".#.", ".#.", ".#.", ".#.", ".#.", "###", "")
g("J", "..###", "....#", "....#", "....#", "#...#", "#...#", ".###.", "")
g("K", "#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#", "")
g("L", "#....", "#....", "#....", "#....", "#....", "#....", "#####", "")
g("M", "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#", "")
g("N", "#...#", "##..#", "#.#.#", "#.#.#", "#..##", "#...#", "#...#", "")
g("O", ".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", "")
g("P", "####.", "#...#", "#...#", "####.", "#....", "#....", "#....", "")
g("Q", ".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#", "")
g("R", "####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#", "")
g("S", ".####", "#....", "#....", ".###.", "....#", "....#", "####.", "")
g("T", "#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "")
g("U", "#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###.", "")
g("V", "#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#..", "")
g("W", "#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#", "")
g("X", "#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#", "")
g("Y", "#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#..", "")
g("Z", "#####", "....#", "...#.", "..#..", ".#...", "#....", "#####", "")

# ------------------------------------------------------------------ lowercase
g("a", "", "", ".###.", "....#", ".####", "#...#", ".####", "")
g("b", "#....", "#....", "####.", "#...#", "#...#", "#...#", "####.", "")
g("c", "", "", ".####", "#....", "#....", "#....", ".####", "")
g("d", "....#", "....#", ".####", "#...#", "#...#", "#...#", ".####", "")
g("e", "", "", ".###.", "#...#", "#####", "#....", ".###.", "")
g("f", "..##", ".#..", ".#..", "####", ".#..", ".#..", ".#..", "")
g("g", "", "", ".####", "#...#", "#...#", ".####", "....#", ".###.")
g("h", "#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#", "")
g("i", "#", "", "#", "#", "#", "#", "#", "")
g("j", ".#", "..", ".#", ".#", ".#", ".#", ".#", "#.")
g("k", "#...", "#..#", "#.#.", "##..", "#.#.", "#..#", "#..#", "")
g("l", "#", "#", "#", "#", "#", "#", "#", "")
g("m", "", "", "##.#.", "#.#.#", "#.#.#", "#.#.#", "#.#.#", "")
g("n", "", "", "####.", "#...#", "#...#", "#...#", "#...#", "")
g("o", "", "", ".###.", "#...#", "#...#", "#...#", ".###.", "")
g("p", "", "", "####.", "#...#", "#...#", "####.", "#....", "#....")
g("q", "", "", ".####", "#...#", "#...#", ".####", "....#", "....#")
g("r", "", "", "#.##.", "##...", "#....", "#....", "#....", "")
g("s", "", "", ".####", "#....", ".###.", "....#", "####.", "")
g("t", ".#..", ".#..", "####", ".#..", ".#..", ".#..", "..##", "")
g("u", "", "", "#...#", "#...#", "#...#", "#...#", ".####", "")
g("v", "", "", "#...#", "#...#", "#...#", ".#.#.", "..#..", "")
g("w", "", "", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#.", "")
g("x", "", "", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "")
g("y", "", "", "#...#", "#...#", "#...#", ".####", "....#", ".###.")
g("z", "", "", "#####", "...#.", "..#..", ".#...", "#####", "")

# Code 127 (DEL, otherwise unused) carries 'e' with an acute: the decomp item text really
# does say "POKéMON", and ui.c maps the UTF-8 sequence C3 A9 onto this slot so the games'
# own spelling survives instead of printing "POK?MON".
g("\x7f", "..#..", ".#...", ".###.", "#...#", "#####", "#....", ".###.", "")


def build():
    """96 glyphs x 8 rows, plus the advance table."""
    data, widths = [], []
    for code in range(32, 128):
        ch = chr(code)
        art = G.get(ch)
        if art is None:                       # unauthored -> a visible box, never blank
            art = ["#####", "#...#", "#...#", "#...#", "#...#", "#####", "", ""]
        ink = max((len(r) for r in art), default=0)
        rows = []
        for r in range(8):
            s = art[r] if r < len(art) else ""
            b = 0
            for i, c in enumerate(s):
                if c == "#":
                    b |= 1 << i               # bit 0 = leftmost pixel
            rows.append(b)
        data.append(rows)
        if ch == " ":
            widths.append(3)                  # word space
        else:
            widths.append(ink + 1)            # ink + 1 px gap
    return data, widths


def emit(data, widths):
    L = []
    L.append("/* GENERATED by tools/gen_font.py -- do not edit by hand.")
    L.append(" *")
    L.append(" * PokeDNA's own proportional 5x7 face. Authored as ASCII art in the generator;")
    L.append(" * not extracted or converted from any ROM or decomp (hard rule 6).")
    L.append(" *")
    L.append(" * 96 glyphs, ASCII 32..127, 8 bytes each, one byte per row, top row first.")
    L.append(" * Bit 0 of a row byte is the LEFTMOST pixel (same order as tonc's sys8).")
    L.append(" * ui_font_w[i] is the ADVANCE: ink width + the 1 px inter-glyph gap. */")
    L.append("#include \"ui_font.h\"")
    L.append("")
    L.append("const unsigned char ui_font_bits[96 * 8] = {")
    for i, rows in enumerate(data):
        ch = chr(32 + i)
        label = "space" if ch == " " else ch
        L.append("  " + ", ".join("0x%02X" % b for b in rows) +
                 ",   /* %-5s */" % label)
    L.append("};")
    L.append("")
    L.append("const unsigned char ui_font_w[96] = {")
    for i in range(0, 96, 16):
        L.append("  " + ", ".join("%d" % w for w in widths[i:i + 16]) + ",")
    L.append("};")
    L.append("")
    return "\n".join(L)


def proof(data, widths, path):
    """Render every glyph to a PNG so the shapes can actually be looked at."""
    from PIL import Image, ImageDraw
    cols, cell = 16, 14
    im = Image.new("RGB", (cols * cell + 8, 6 * cell + 40), (20, 26, 40))
    d = ImageDraw.Draw(im)
    for i, rows in enumerate(data):
        cx, cy = 4 + (i % cols) * cell, 4 + (i // cols) * cell
        for r, b in enumerate(rows):
            for c in range(8):
                if b >> c & 1:
                    d.point((cx + c, cy + r), (230, 236, 245))
        d.point((cx + widths[i] - 1, cy + 8), (255, 80, 60))    # advance mark
    y = 6 * cell + 8
    for n, s in enumerate(["SUPER POTION x94", "No01 FOCUS PUNCH", "CHESTO BERRY", "JIGGLYPUFF"]):
        x = 4
        for chx in s:
            gi = ord(chx) - 32
            if 0 <= gi < 96:
                for r, b in enumerate(data[gi]):
                    for c in range(8):
                        if b >> c & 1:
                            d.point((x + c, y + r), (255, 220, 120))
                x += widths[gi]
        d.point((x, y + 7), (255, 80, 60))
        y += 10
    im.resize((im.width * 4, im.height * 4), Image.NEAREST).save(path)
    print("proof ->", path)


if __name__ == "__main__":
    data, widths = build()
    if "--proof" in sys.argv:
        proof(data, widths, sys.argv[sys.argv.index("--proof") + 1])
    else:
        with open(OUT, "w") as f:
            f.write(emit(data, widths))
        wide = max(widths)
        print("wrote", OUT)
        print("  advance: min %d  max %d  'A'=%d  'SUPER POTION'=%d px  'No01 FOCUS PUNCH'=%d px"
              % (min(w for w in widths), wide, widths[ord("A") - 32],
                 sum(widths[ord(c) - 32] for c in "SUPER POTION"),
                 sum(widths[ord(c) - 32] for c in "No01 FOCUS PUNCH")))
