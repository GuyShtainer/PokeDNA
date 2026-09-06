#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""gbicon_sheet.py — visual proof for E5 (docs/SPRITE-ERA-DESIGN.md sec 2/4): every
Gen-2 party/PC menu icon (frame 0, 16x16), decoded straight off Guy's own Gold.gbc
and Crystal.gbc, laid out into one 4x-scaled contact sheet per ROM.

WHY A SEPARATE SCRIPT, NOT A MODE OF tools/fuse_sav.py
--------------------------------------------------------
tools/fuse_sav.py fuses a .sav (+ --gb + --clip) into pokedna-delta.gba for the
emulator, but it has NO ROM-payload path at all — the emulator session never has
a second file to register as "the Gen-2 cartridge" (source/gb_art_source.c's
gb_art_register()/gb_rom_path_beside() both need an actual FatFs-visible file,
which the emulator's single fused image cannot provide). Building one would be a
new feature this slice does not need: this proof only has to show what the real
menu icons LOOK like, decoded from Guy's real dumps — the same thing
tests/host_romgbicon_test.c already asserts byte-for-byte, just rendered instead
of hashed. So this reads the ROM directly on the host, exactly like every other
tools/gen_*.py in this project that parses ROM data in Python.

INDEPENDENCE FROM source/rom_gbicon.c
---------------------------------------
This is a SEPARATE, from-scratch re-implementation of the same shape-based
locator (see source/rom_gbicon.h's own long comment for the full citation trail
and the invariants), not a wrapper around the C module or a call into the host
test binary. Two independent implementations agreeing on the same real ROM bytes
is a stronger proof than one implementation rendering its own output.

CLEAN-ROOM: same posture as rom_gbicon.h -- pokecrystal/pokegold were read on the
web for the LAYOUT only; every offset here is independently located BY SHAPE in
Guy's own dumps, never pinned to a decomp address, and this script embeds no
Nintendo/Game Freak pixel data of its own (it decodes the USER'S OWN ROM file at
run time and writes nothing but a rendering of it to disk, exactly like every
other screenshot/sheet tool in this project).

Usage (needs Pillow; SKIPS a game whose dump is not present):
    /usr/local/bin/python3 tools/gbicon_sheet.py

Writes:
    docs/shots/gb/e5_01_gold_icons.png
    docs/shots/gb/e5_02_crystal_icons.png
and appends both to docs/shots/gb/manifest.json (gb_contact_sheet.py's format)
under the "E5:" caption prefix, so `gb_contact_sheet.py --per-feature` picks them
up as the "e5-icons" feature.
"""
from __future__ import annotations

import json
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
ROMS = Path("/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb")
OUT_DIR = ROOT / "docs" / "shots" / "gb"
MANIFEST = OUT_DIR / "manifest.json"

GAMES = [
    ("Gold.gbc", "e5_01_gold_icons.png", "Gold"),
    ("Crystal.gbc", "e5_02_crystal_icons.png", "Crystal"),
]

GB_BANK = 0x4000
SPECIES = 251


def fnv1a(data: bytes) -> int:
    h = 0x811C9DC5
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def parse_header(rom: bytes) -> dict | None:
    """Fail-closed identity check -- the same 48-byte boot-logo fingerprint and
    header checksum source/rom_gbsprite.c/rom_gbicon.c already use (measured
    identically on all four of Guy's dumps, not decomp data)."""
    if len(rom) < 0x150:
        return None
    logo = rom[0x104:0x134]
    if fnv1a(logo) != 0x016BAD3F:
        return None
    x = 0
    for b in rom[0x134:0x14D]:
        x = (x - b - 1) & 0xFF
    if x != rom[0x14D]:
        return None
    code = rom[0x148]
    if code > 8 or len(rom) != (0x8000 << code):
        return None
    return {"banks": len(rom) // GB_BANK}


def find_menu_icons(rom: bytes) -> tuple[int, int] | None:
    """MonMenuIcons: 251 bytes, each in [1,63], plus the ten evolution-line
    structural invariants (see rom_gbicon.h). Returns (offset, n) or None."""
    n = len(rom)
    best = None
    for off in range(0, n - SPECIES):
        w = rom[off:off + SPECIES]
        mx = 0
        ok = True
        seen = set()
        for b in w:
            if b == 0 or b > 63:
                ok = False
                break
            if b > mx:
                mx = b
            seen.add(b)
        if not ok or len(seen) < 8:
            continue
        if not (w[0] == w[1] == w[2]):
            continue
        if w[3] != w[4] or w[3] == w[5]:
            continue
        if not (w[6] == w[7] == w[8]):
            continue
        if w[9] != w[10] or w[10] == w[11]:
            continue
        if not (w[15] == w[16] == w[17]):
            continue
        if w[22] != w[23]:
            continue
        if w[26] != w[27]:
            continue
        if not (w[42] == w[43] == w[44]):
            continue
        if w[49] != w[50]:
            continue
        if best is not None:
            return None  # ambiguous -- fail closed
        best = (off, mx)
    return best


def find_icon_pointers(rom: bytes, n: int) -> int | None:
    """IconPointers: a MAXIMAL run of n+1 little-endian u16 entries, entry[0] ==
    entry[1], entry[i+1] == entry[i] + 128 for i in 1..n-1. Returns the offset or
    None; fails closed if the run is not unique."""
    best = None
    limit = len(rom) - (n + 2) * 2
    for off in range(0, limit):
        v0 = rom[off] | (rom[off + 1] << 8)
        v1 = rom[off + 2] | (rom[off + 3] << 8)
        if v0 != v1 or not (0x4000 <= v0 < 0x8000):
            continue
        prev = v1
        ok = True
        for i in range(1, n):
            cur = rom[off + (i + 1) * 2] | (rom[off + (i + 1) * 2 + 1] << 8)
            if cur != (prev + 128) & 0xFFFF:
                ok = False
                break
            prev = cur
        if not ok:
            continue
        ext = rom[off + (n + 1) * 2] | (rom[off + (n + 1) * 2 + 1] << 8)
        if ext == (prev + 128) & 0xFFFF:
            continue  # not maximal -- some longer run continues past n
        if best is not None:
            return None
        best = off
    return best


def tile_2bpp(t: bytes) -> list[list[int]]:
    px = [[0] * 8 for _ in range(8)]
    for r in range(8):
        lo, hi = t[r * 2], t[r * 2 + 1]
        for c in range(8):
            bit = 7 - c
            px[r][c] = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1)
    return px


# D2 (E5 fix): NOT the DMG monochrome ramp -- Gen 2 colours every menu icon with the
# fixed party-menu OBJ palette 0 (PartyMenuOBPals, gfx/stats/party_menu_ob.pal, byte-
# identical in pokegold and pokecrystal): idx0 RGB(27,31,27) transparent, idx1
# RGB(31,19,10) light orange, idx2 RGB(31,7,4) red, idx3 RGB(0,0,0) black. These are
# GBC 5-bit-per-channel values (0..31); scaled to 8-bit (v*255//31) for this PNG
# sheet, the same four colours source/rom_gbicon.c's GB_ICON_PAL packs as RGB15.
_PAL_5BIT = {0: (27, 31, 27), 1: (31, 19, 10), 2: (31, 7, 4), 3: (0, 0, 0)}
RAMP = {k: tuple(v * 255 // 31 for v in rgb) for k, rgb in _PAL_5BIT.items()}


def decode_frame0(rom: bytes, off: int) -> Image.Image:
    tiles = [tile_2bpp(rom[off + t * 16: off + t * 16 + 16]) for t in range(4)]
    pos = [(0, 0), (0, 8), (8, 0), (8, 8)]
    im = Image.new("RGB", (16, 16))
    px = im.load()
    for tile, (ty, tx) in zip(tiles, pos):
        for r in range(8):
            for c in range(8):
                px[tx + c, ty + r] = RAMP[tile[r][c]]
    return im


def locate_and_decode(rom_path: Path) -> tuple[list[Image.Image], int, int, int]:
    rom = rom_path.read_bytes()
    hdr = parse_header(rom)
    if hdr is None:
        raise ValueError(f"{rom_path.name}: not a recognisable Game Boy ROM")
    hit = find_menu_icons(rom)
    if hit is None:
        raise ValueError(f"{rom_path.name}: MonMenuIcons shape not found (or ambiguous) "
                          "-- expected for a real Gen-1 ROM (no menu-icon shape), a bug "
                          "otherwise")
    menu_off, n = hit
    ptr_off = find_icon_pointers(rom, n)
    if ptr_off is None:
        raise ValueError(f"{rom_path.name}: IconPointers run not found for n={n}")
    bank = ptr_off // GB_BANK
    icons = []
    for k in range(1, n + 1):
        v = rom[ptr_off + k * 2] | (rom[ptr_off + k * 2 + 1] << 8)
        foff = bank * GB_BANK + (v - 0x4000)
        icons.append(decode_frame0(rom, foff))
    return icons, menu_off, ptr_off, bank


def build_sheet(icons: list[Image.Image], title: str) -> Image.Image:
    scale = 4
    cell = 16 * scale
    cols = 8
    rows = (len(icons) + cols - 1) // cols
    pad = 4
    header_h = 20
    w = cols * (cell + pad) + pad
    h = header_h + rows * (cell + pad) + pad
    sheet = Image.new("RGB", (w, h), (13, 17, 23))
    for i, icon in enumerate(icons):
        rgb = icon.convert("RGB").resize((cell, cell), Image.NEAREST)
        x = pad + (i % cols) * (cell + pad)
        y = header_h + pad + (i // cols) * (cell + pad)
        sheet.paste(rgb, (x, y))
    return sheet


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    manifest = json.loads(MANIFEST.read_text()) if MANIFEST.exists() else {"shots": []}
    existing_files = {s["file"] for s in manifest["shots"]}
    wrote_any = False

    for rom_name, out_name, label in GAMES:
        rom_path = ROMS / rom_name
        if not rom_path.exists():
            print(f"  SKIP {rom_name} (no {rom_path})")
            continue
        icons, menu_off, ptr_off, bank = locate_and_decode(rom_path)
        n = len(icons)
        sheet = build_sheet(icons, label)
        out_path = OUT_DIR / out_name
        sheet.save(out_path)
        wrote_any = True
        print(f"  wrote {out_path} ({n} icons, MonMenuIcons=0x{menu_off:X} "
              f"IconPointers=0x{ptr_off:X} bank=0x{bank:X})")

        caption = (f"E5: {label} — every one of the {n} real Gen-2 party/PC menu icons "
                   f"(frame 0, 16x16, 4x scaled), decoded live from Guy's own {rom_name} "
                   f"by an independent from-scratch locator (tools/gbicon_sheet.py) as a "
                   f"cross-check against source/rom_gbicon.c's own shape-based one -- "
                   f"MonMenuIcons=0x{menu_off:X} IconPointers=0x{ptr_off:X} bank=0x{bank:X}")
        if out_name not in existing_files:
            manifest["shots"].append({"file": out_name, "caption": caption})
        else:
            for s in manifest["shots"]:
                if s["file"] == out_name:
                    s["caption"] = caption

    if wrote_any:
        MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"  updated {MANIFEST}")
    else:
        print("NOTHING WRITTEN (no Gen-2 ROMs present)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
