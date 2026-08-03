#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""fuse_rom.py — append a Pokemon ROM onto a PokeDNA build so both live in one
cartridge image, and patch the locator record so the running code can find it.

WHY THIS EXISTS
---------------
On real hardware PokeDNA reads the player's game ROM as a *file* off the
flashcart's microSD. An emulated GBA has no flashcart and no microSD: mGBA
hands the program exactly one cartridge and nothing else. So under an emulator
the map viewer (and anything else that needs to read tilesets/maps out of the
commercial game) has no file to open and simply cannot work.

Fusing sidesteps that. The Pokemon ROM is appended to the end of the PokeDNA
image, and a small locator record inside PokeDNA is patched with where it
landed. The GBA maps the whole cartridge at 0x08000000, so at run time the
appended ROM is readable as plain memory at

    0x08000000 + offset

and "load a chunk of the game ROM" collapses from an SD/FatFs transaction into
a memcpy. Same code path works in mGBA and on hardware (on hardware the fused
image is just a bigger .gba), which is exactly what makes automated emulator
testing of ROM-reading features possible.

THE LOCATOR RECORD (16 bytes, 4-byte aligned, defined by the C side)

    offset  size  field
    +0       8    magic  : ASCII "PDNAFUSE", no NUL terminator
    +8       4    offset : u32 little-endian, byte offset of the appended ROM
                           from the START of the fused file
    +12      4    size   : u32 little-endian, byte length of the appended ROM

In an unfused PokeDNA build both offset and size are 0, which is how the C side
knows there is no fused ROM and falls back to the SD path.

    python3 tools/fuse_rom.py <pokedna.gba> <pokemon.gba> -o <fused.gba>
    python3 tools/fuse_rom.py --check <fused.gba>

!!  LEGAL  !!
The output contains a verbatim copy of a commercial Nintendo/Game Freak game
ROM. It is a personal-use test artifact ONLY. NEVER commit it, never attach it
to a GitHub release, never publish or share it, and keep it out of any repo
(add the fused filename to .gitignore). Distributing it would be straight
copyright infringement — unlike PokeDNA itself, which ships no game data.
"""

from __future__ import annotations

import argparse
import os
import sys

MAGIC = b"PDNAFUSE"                 # 8 bytes, no NUL
RECORD_SIZE = 16                    # magic(8) + offset(4) + size(4)
ALIGN = 256                         # appended ROM starts on a 256-byte boundary
CART_WINDOW = 0x02000000            # 32 MiB — the GBA's cart address space
CART_BASE = 0x08000000

# Gen-3 game codes at header offset 0xAC. 0xB2 == 0x96 is the GBA header's
# mandatory fixed byte.
GEN3_CODES = {b"BPEE": "Emerald", b"AXVE": "Ruby", b"AXPE": "Sapphire",
              b"BPRE": "FireRed", b"BPGE": "LeafGreen"}
HDR_FIXED_OFF = 0xB2
HDR_CODE_OFF = 0xAC


class FuseError(Exception):
    """Anything that should stop the tool with a clear message."""


# --------------------------------------------------------------------------
# record helpers
# --------------------------------------------------------------------------
def find_records(blob: bytes) -> list[int]:
    """Every offset at which MAGIC occurs (any alignment)."""
    hits, start = [], 0
    while True:
        i = blob.find(MAGIC, start)
        if i < 0:
            return hits
        hits.append(i)
        start = i + 1


def locate_record(blob: bytes, what: str) -> int:
    """Offset of THE one locator record; raises unless it occurs exactly once."""
    hits = find_records(blob)
    if not hits:
        raise FuseError(
            f"{what}: no {MAGIC.decode()} locator record found.\n"
            "  This build has no fuse record — the C side must define the\n"
            "  16-byte record (magic + u32 offset + u32 size) and it must not\n"
            "  be optimised away (mark it used/volatile, e.g. \n"
            "  __attribute__((used, section(\".rodata\"), aligned(4)))).")
    if len(hits) > 1:
        listing = ", ".join(f"0x{h:X}" for h in hits)
        raise FuseError(
            f"{what}: {len(hits)} {MAGIC.decode()} records found at {listing}.\n"
            "  Exactly one is required — fusing cannot know which to patch.\n"
            "  Usually the magic string got duplicated (a separate string\n"
            "  literal as well as the record). Build the magic into the record\n"
            "  only, e.g. as a char array initialiser, not a shared literal.")
    off = hits[0]
    if off % 4:
        raise FuseError(
            f"{what}: locator record at 0x{off:X} is not 4-byte aligned.\n"
            "  The C side declares it aligned(4); a misaligned hit means this\n"
            "  is not the real record (stray copy of the magic in data?).")
    return off


def read_record(blob: bytes, rec_off: int) -> tuple[int, int]:
    off = int.from_bytes(blob[rec_off + 8:rec_off + 12], "little")
    size = int.from_bytes(blob[rec_off + 12:rec_off + 16], "little")
    return off, size


def write_record(buf: bytearray, rec_off: int, off: int, size: int) -> None:
    buf[rec_off + 8:rec_off + 12] = off.to_bytes(4, "little")
    buf[rec_off + 12:rec_off + 16] = size.to_bytes(4, "little")


# --------------------------------------------------------------------------
# sanity checks
# --------------------------------------------------------------------------
def describe_gba_header(blob: bytes) -> tuple[bool, str]:
    """(looks_like_gen3, human description) for a .gba image."""
    if len(blob) < 0xC0:
        return False, "shorter than a GBA header (192 bytes)"
    fixed = blob[HDR_FIXED_OFF]
    code = bytes(blob[HDR_CODE_OFF:HDR_CODE_OFF + 4])
    title = bytes(blob[0xA0:0xAC]).rstrip(b"\x00 ").decode("ascii", "replace")
    ok_fixed = fixed == 0x96
    game = GEN3_CODES.get(code)
    desc = (f"title={title!r} code={code.decode('ascii', 'replace')!r} "
            f"fixed[0xB2]=0x{fixed:02X}"
            + (f" -> Pokemon {game}" if game else ""))
    return (ok_fixed and game is not None), desc


def warn_if_not_gen3(blob: bytes, path: str) -> None:
    ok, desc = describe_gba_header(blob)
    if ok:
        print(f"  appended ROM: {desc}")
        return
    print(f"  WARNING: {os.path.basename(path)} does not look like a Gen-3 "
          f"Pokemon ROM ({desc}).", file=sys.stderr)
    print(f"  WARNING: expected header byte 0xB2 == 0x96 and a game code at "
          f"0xAC in {'/'.join(c.decode() for c in GEN3_CODES)}. "
          "Fusing anyway.", file=sys.stderr)


# --------------------------------------------------------------------------
# fuse
# --------------------------------------------------------------------------
def fuse(pokedna_path: str, pokemon_path: str, out_path: str,
         force: bool = False) -> int:
    for p in (pokedna_path, pokemon_path):
        if not os.path.isfile(p):
            raise FuseError(f"input not found: {p}")

    base = bytearray(open(pokedna_path, "rb").read())
    game = open(pokemon_path, "rb").read()
    if not game:
        raise FuseError(f"{pokemon_path} is empty")

    print(f"PokeDNA : {pokedna_path} ({len(base):,} bytes, "
          f"{len(base)/2**20:.2f} MiB)")
    print(f"Pokemon : {pokemon_path} ({len(game):,} bytes, "
          f"{len(game)/2**20:.2f} MiB)")

    # The magic must be unique in the FUSED file too, so refuse up front if the
    # game ROM happens to contain it (rather than failing after writing).
    if MAGIC in game:
        raise FuseError(
            f"{pokemon_path} itself contains the bytes {MAGIC.decode()} at "
            f"0x{game.find(MAGIC):X}.\n"
            "  Fusing would create two locator records and the C side could "
            "not tell them apart.")

    rec_off = locate_record(bytes(base), os.path.basename(pokedna_path))
    cur_off, cur_size = read_record(bytes(base), rec_off)
    print(f"  locator record at file offset 0x{rec_off:X} "
          f"(cart address 0x{CART_BASE + rec_off:08X})")

    if cur_off or cur_size:
        if not force:
            raise FuseError(
                f"{pokedna_path} is ALREADY fused (record says offset "
                f"0x{cur_off:X}, size {cur_size:,}).\n"
                "  Fuse from the clean unfused PokeDNA build, or pass --force "
                "to truncate the old payload and re-append.")
        if cur_off > len(base):
            raise FuseError(
                f"--force: recorded offset 0x{cur_off:X} is past the end of "
                f"the file ({len(base):,} bytes) — record is corrupt.")
        print(f"  --force: truncating old payload at 0x{cur_off:X} "
              f"({len(base) - cur_off:,} bytes dropped)")
        del base[cur_off:]

    # Pad with 0xFF (erased-flash value) up to the alignment boundary.
    pad = (-len(base)) % ALIGN
    if pad:
        base.extend(b"\xff" * pad)
    offset = len(base)
    assert offset % ALIGN == 0

    total = offset + len(game)
    if total > CART_WINDOW:
        raise FuseError(
            f"fused image would be {total:,} bytes "
            f"({total/2**20:.2f} MiB) — over the {CART_WINDOW/2**20:.0f} MiB "
            "GBA cartridge address window.\n"
            f"  The appended ROM must be addressable at 0x{CART_BASE:08X} + "
            "offset, so nothing beyond 32 MiB can ever be reached.\n"
            "  Shrink the PokeDNA build (trimmed assets) or use a smaller "
            "game ROM.")

    base.extend(game)
    write_record(base, rec_off, offset, len(game))

    tmp = out_path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(base)
    os.replace(tmp, out_path)

    # -------------------------------------------------- verify by re-reading
    check = open(out_path, "rb").read()
    problems = []
    if len(check) != total:
        problems.append(f"size is {len(check):,}, expected {total:,}")
    rec2 = locate_record(check, "fused image")
    if rec2 != rec_off:
        problems.append(f"record moved: 0x{rec2:X} != 0x{rec_off:X}")
    got_off, got_size = read_record(check, rec2)
    if got_off != offset:
        problems.append(f"record offset is 0x{got_off:X}, expected 0x{offset:X}")
    if got_size != len(game):
        problems.append(f"record size is {got_size:,}, expected {len(game):,}")
    if check[offset:offset + len(game)] != game:
        problems.append("appended bytes do NOT match the input ROM")
    if pad and check[offset - pad:offset] != b"\xff" * pad:
        problems.append("padding is not 0xFF")
    if problems:
        raise FuseError("VERIFY FAILED on the written file:\n  - "
                        + "\n  - ".join(problems))

    warn_if_not_gen3(game, pokemon_path)
    print()
    print(f"FUSED -> {out_path}")
    print(f"  total size   : {total:,} bytes ({total/2**20:.2f} MiB)")
    print(f"  padding      : {pad} bytes of 0xFF")
    print(f"  ROM offset   : 0x{offset:08X} ({offset:,})")
    print(f"  ROM size     : 0x{len(game):08X} ({len(game):,})")
    print(f"  cart address : 0x{CART_BASE + offset:08X} .. "
          f"0x{CART_BASE + total - 1:08X}")
    print("  VERIFY: OK — record patched and appended bytes match byte-for-byte")
    if total > 16 * 2**20:
        print("  NOTE: over 16 MiB. mGBA is fine with it, but a flashcart's "
              "SD loader may not be —")
        print("        see the toolkit's rom-load-lab findings before "
              "expecting this to boot from SD.")
    print("  REMINDER: this file embeds a commercial game ROM. Do not commit, "
          "publish or share it.")
    return 0


# --------------------------------------------------------------------------
# check
# --------------------------------------------------------------------------
def check_only(path: str) -> int:
    if not os.path.isfile(path):
        raise FuseError(f"file not found: {path}")
    blob = open(path, "rb").read()
    print(f"{path} ({len(blob):,} bytes, {len(blob)/2**20:.2f} MiB)")
    rec_off = locate_record(blob, os.path.basename(path))
    off, size = read_record(blob, rec_off)
    print(f"  locator record at 0x{rec_off:X} "
          f"(cart address 0x{CART_BASE + rec_off:08X})")
    if off == 0 and size == 0:
        print("  offset=0 size=0 -> NOT FUSED (clean PokeDNA build)")
        return 0
    print(f"  offset : 0x{off:08X} ({off:,})  -> cart 0x{CART_BASE + off:08X}")
    print(f"  size   : 0x{size:08X} ({size:,} bytes, {size/2**20:.2f} MiB)")
    bad = []
    if off % ALIGN:
        bad.append(f"offset is not {ALIGN}-byte aligned")
    if off + size > len(blob):
        bad.append(f"offset+size ({off + size:,}) runs past the end of the file")
    if off + size > CART_WINDOW:
        bad.append("payload extends past the 32 MiB cart window")
    if bad:
        raise FuseError("record is INCONSISTENT:\n  - " + "\n  - ".join(bad))
    payload = blob[off:off + size]
    ok, desc = describe_gba_header(payload)
    print(f"  payload: {desc}")
    print("  " + ("payload looks like a Gen-3 Pokemon ROM" if ok else
                  "WARNING: payload does not look like a Gen-3 Pokemon ROM"))
    print("  record is CONSISTENT")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog="The fused output embeds a commercial game ROM: personal use "
               "only, never commit or share it.",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pokedna", nargs="?", help="the PokeDNA .gba build")
    ap.add_argument("pokemon", nargs="?", help="the Pokemon .gba to append")
    ap.add_argument("-o", "--output", help="fused output .gba")
    ap.add_argument("--check", metavar="FUSED.GBA",
                    help="just report the locator record of an image")
    ap.add_argument("--force", action="store_true",
                    help="re-fuse an already-fused image (drops the old payload)")
    a = ap.parse_args(argv)

    try:
        if a.check:
            if a.pokedna or a.pokemon or a.output:
                ap.error("--check takes no other arguments")
            return check_only(a.check)
        if not (a.pokedna and a.pokemon):
            ap.error("need <pokedna.gba> <pokemon.gba> -o <fused.gba> "
                     "(or --check <fused.gba>)")
        if not a.output:
            ap.error("-o/--output is required")
        if os.path.abspath(a.output) in (os.path.abspath(a.pokedna),
                                         os.path.abspath(a.pokemon)):
            ap.error("--output would overwrite an input file")
        return fuse(a.pokedna, a.pokemon, a.output, force=a.force)
    except FuseError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
