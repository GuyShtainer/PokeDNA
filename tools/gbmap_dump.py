#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""gbmap_dump.py -- BACKLOG #91 M1. Independent host-side re-derivation of
source/rom_gbmap.c's Gen-1 (Red/Blue/Yellow) map locators, used as this
slice's own oracle: prints the 3 located tables (MapHeaderBanks/
MapHeaderPointers/Tilesets) plus the given map's own header fields, and
renders the WHOLE current map to a PNG (the tile-decode half rom_gbmap.c's
own C code also performs, done here independently in Python so a byte-level
divergence between the two implementations is visible as a wrong pixel, not
hidden by both sides sharing one buggy routine).

Same shape convention as tools/gbui_dump.py (a host-side dump/oracle tool
for a rom_gb*.c locator), but a from-scratch re-implementation rather than a
C-shim wrapper: rom_gbmap.c's own anchors (see rom_gbmap.h's design comment)
are short enough to re-derive directly in Python, the same way this slice's
own research pass first proved them out before writing any C.

Usage:
  python3 tools/gbmap_dump.py --rom Red.gb [--sav Red.sav] [--map-id N]
                               [--out map.png]

With --sav (and no --map-id), the CURRENT map is whichever map id the save's
own $A60A byte names (Red/Yellow share this SRAM offset -- BACKLOG #91's
design grilling section, cross-checked live in tests/host_romgbmap_test.c).
Without --sav, --map-id defaults to 0 (Pallet Town).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    Image = None

GB_BANK = 0x4000
GB_WIN_LO = 0x4000

# DMG 4-shade ramp, same 8-bit values rom_gbui.c's DMG_SHADE / pdna_gbscreen.c's
# kGbscrPicShade use (GB8_TO_RGB15's own inputs) -- kept as plain 0..255 greys
# here since this is a host PNG, not a GBA RGB15 framebuffer.
SHADE = [0xF8, 0xA8, 0x58, 0x10]


def fileoff(bank: int, addr: int) -> int:
    if addr < GB_WIN_LO:
        return addr
    return bank * GB_BANK + (addr - GB_WIN_LO)


def find_all(data: bytes, pat: list) -> list[int]:
    n = len(pat)
    hits = []
    for i in range(len(data) - n + 1):
        ok = True
        for j, b in enumerate(pat):
            if b is not None and data[i + j] != b:
                ok = False
                break
        if ok:
            hits.append(i)
    return hits


def locate(rom: bytes) -> dict:
    """Re-derives the same 3 anchors rom_gbmap.c's rgm1_open() uses -- see
    rom_gbmap.h's design comment for the exact byte shapes."""
    # MapHeaderBanks: E5 C5 4F 06 00 3E bb CD ll hh 21 lo hi 09
    pat_banks = [0xE5, 0xC5, 0x4F, 0x06, 0x00, 0x3E, None, 0xCD, None, None, 0x21, None, None, 0x09]
    hb = find_all(rom, pat_banks)
    if len(hb) != 1:
        raise ValueError(f"MapHeaderBanks anchor: expected 1 hit, got {len(hb)} (ambiguous or missing)")
    x = hb[0]
    banks_off = fileoff(rom[x + 6], rom[x + 11] | (rom[x + 12] << 8))

    # MapHeaderPointers: CB 78 C0, then branch on the next byte.
    pat_hdr = [0xCB, 0x78, 0xC0]
    hh = find_all(rom, pat_hdr)
    if len(hh) != 1:
        raise ValueError(f"MapHeaderPointers anchor: expected 1 hit, got {len(hh)} (ambiguous or missing)")
    x = hh[0]
    nb = rom[x + 3]
    if nb == 0x21:
        addr = rom[x + 4] | (rom[x + 5] << 8)
        if addr >= GB_WIN_LO:
            raise ValueError("MapHeaderPointers: direct case, addr not in home bank")
        ptrs_off = addr
    elif nb == 0xCD:
        target = rom[x + 4] | (rom[x + 5] << 8)
        if target >= GB_WIN_LO:
            raise ValueError("MapHeaderPointers: indirect case, call target not in home bank")
        bank = None
        addr = None
        i = target
        while i < target + 48:
            if rom[i] == 0x3E and rom[i + 2] == 0xCD:
                bank = rom[i + 1]
            if bank is not None and rom[i] == 0x21:
                addr = rom[i + 1] | (rom[i + 2] << 8)
                break
            i += 1
        if bank is None or addr is None:
            raise ValueError("MapHeaderPointers: indirect case, sub-scan found no bank+addr")
        ptrs_off = fileoff(bank, addr)
    else:
        raise ValueError(f"MapHeaderPointers: unexpected byte after CB 78 C0: {nb:#x}")

    # Tilesets: 5F 21 lo hi, filtered by one-or-more ADD HL,DE then LD DE,<WRAM>.
    pat_ts = [0x5F, 0x21, None, None]
    valid = []
    for x in find_all(rom, pat_ts):
        addr = rom[x + 2] | (rom[x + 3] << 8)
        j = x + 4
        n19 = 0
        while j < x + 8 and rom[j] == 0x19:
            n19 += 1
            j += 1
        if n19 < 1 or rom[j] != 0x11:
            continue
        wram = rom[j + 1] | (rom[j + 2] << 8)
        if not (0xC000 <= wram < 0xE000):
            continue
        valid.append((x // GB_BANK, addr))
    if len(valid) != 1:
        raise ValueError(f"Tilesets anchor: expected 1 surviving candidate, got {len(valid)} (ambiguous or missing)")
    ts_bank, ts_addr = valid[0]
    tilesets_off = fileoff(ts_bank, ts_addr)

    return dict(banks_off=banks_off, ptrs_off=ptrs_off, tilesets_off=tilesets_off)


def parse_header(rom: bytes, loc: dict, map_id: int) -> dict:
    bank = rom[loc["banks_off"] + map_id]
    ptr = rom[loc["ptrs_off"] + map_id * 2] | (rom[loc["ptrs_off"] + map_id * 2 + 1] << 8)
    hdr_off = fileoff(bank, ptr)
    tileset_id, height, width = rom[hdr_off], rom[hdr_off + 1], rom[hdr_off + 2]
    blocks_ptr = rom[hdr_off + 3] | (rom[hdr_off + 4] << 8)
    conn_mask = rom[hdr_off + 9]
    nconn = bin(conn_mask).count("1")
    conns = []
    p = hdr_off + 10
    for _ in range(nconn):
        conns.append(dict(map_id=rom[p], width=rom[p + 6], y=rom[p + 7], x=rom[p + 8]))
        p += 11
    obj_ptr = rom[p] | (rom[p + 1] << 8)
    blocks_off = fileoff(bank, blocks_ptr)
    return dict(map_id=map_id, bank=bank, hdr_off=hdr_off, tileset_id=tileset_id,
                height=height, width=width, blocks_off=blocks_off,
                conn_mask=conn_mask, conns=conns, obj_ptr=obj_ptr)


def parse_tileset(rom: bytes, loc: dict, tileset_id: int) -> dict:
    off = loc["tilesets_off"] + tileset_id * 12
    row = rom[off:off + 12]
    gfx_bank = row[0]
    block_ptr = row[1] | (row[2] << 8)
    gfx_ptr = row[3] | (row[4] << 8)
    return dict(gfx_bank=gfx_bank, block_off=fileoff(gfx_bank, block_ptr),
                gfx_off=fileoff(gfx_bank, gfx_ptr))


def render_map(rom: bytes, hdr: dict, ts: dict) -> "Image.Image":
    """Whole-map render: each block (4x4 tiles, 32x32 px) resolved through the
    tileset's own blockset, each tile decoded from raw planar 2bpp (uncompressed --
    see rom_gbmap.h's own note that Gen-1 tile graphics are INCBIN, no RLE)."""
    w_px, h_px = hdr["width"] * 32, hdr["height"] * 32
    img = Image.new("L", (w_px, h_px))
    px = img.load()
    tile_cache: dict[int, list[int]] = {}

    def decode_tile(tile_id: int) -> list[int]:
        cached = tile_cache.get(tile_id)
        if cached is not None:
            return cached
        toff = ts["gfx_off"] + tile_id * 16
        raw = rom[toff:toff + 16]
        out = [0] * 64
        for ry in range(8):
            lo, hi = raw[ry * 2], raw[ry * 2 + 1]
            for cx in range(8):
                b = 7 - cx
                idx = ((lo >> b) & 1) | (((hi >> b) & 1) << 1)
                out[ry * 8 + cx] = SHADE[idx]
        tile_cache[tile_id] = out
        return out

    for by in range(hdr["height"]):
        for bx in range(hdr["width"]):
            block_id = rom[hdr["blocks_off"] + by * hdr["width"] + bx]
            boff = ts["block_off"] + block_id * 16
            tile_ids = rom[boff:boff + 16]
            for ty in range(4):
                for tx in range(4):
                    tile_id = tile_ids[ty * 4 + tx]
                    tpx = decode_tile(tile_id)
                    ox, oy = bx * 32 + tx * 8, by * 32 + ty * 8
                    for ry in range(8):
                        for rx in range(8):
                            px[ox + rx, oy + ry] = tpx[ry * 8 + rx]
    return img


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", type=Path, required=True)
    ap.add_argument("--sav", type=Path, help="Red.sav/Yellow.sav -- picks the CURRENT map automatically")
    ap.add_argument("--map-id", type=int, default=None, help="override (or use without --sav; default 0)")
    ap.add_argument("--out", type=Path, help="PNG output path (skipped if Pillow is unavailable)")
    a = ap.parse_args(argv)

    rom = a.rom.read_bytes()
    loc = locate(rom)
    print(f"MapHeaderBanks    @ {loc['banks_off']:#08x}")
    print(f"MapHeaderPointers @ {loc['ptrs_off']:#08x}")
    print(f"Tilesets          @ {loc['tilesets_off']:#08x}")

    map_id = a.map_id
    px = py = None
    if map_id is None:
        if a.sav:
            sav = a.sav.read_bytes()
            map_id, py, px = sav[0x260A], sav[0x260D], sav[0x260E]
            print(f"player: map_id={map_id} ({map_id:#04x}) x={px} y={py} (from {a.sav.name})")
        else:
            map_id = 0

    hdr = parse_header(rom, loc, map_id)
    print(f"\nmap id {map_id}: tileset_id={hdr['tileset_id']} "
          f"{hdr['width']}x{hdr['height']} blocks, bank={hdr['bank']}, "
          f"hdr_off={hdr['hdr_off']:#08x}, blocks_off={hdr['blocks_off']:#08x}")
    print(f"connections ({hdr['conn_mask']:#04b}): {hdr['conns']}")
    print(f"Object ptr: {hdr['obj_ptr']:#06x}")

    ts = parse_tileset(rom, loc, hdr["tileset_id"])
    print(f"tileset {hdr['tileset_id']}: gfx_bank={ts['gfx_bank']} "
          f"block_off={ts['block_off']:#08x} gfx_off={ts['gfx_off']:#08x}")

    if a.out:
        if Image is None:
            print("(Pillow not installed -- skipping PNG render)")
        else:
            img = render_map(rom, hdr, ts)
            a.out.parent.mkdir(parents=True, exist_ok=True)
            img.save(a.out)
            print(f"\nrendered {img.width}x{img.height} px -> {a.out}")
            if px is not None:
                print(f"player pixel position (approx, wXCoord/wYCoord are 2 tiles/unit): "
                      f"({px * 16}, {py * 16})")

    return 0


if __name__ == "__main__":
    sys.exit(main())
