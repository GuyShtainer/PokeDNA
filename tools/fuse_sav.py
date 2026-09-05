#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""fuse_sav.py — append a Pokemon .sav onto a PokeDNA build so the emulator
build needs nothing but the .gba, and patch the locator record so the running
code can find it.

WHY THIS EXISTS
---------------
The emulator build (PDNA_TARGET=delta) edits its own 128 KiB flash chip. That
means the user has to place their .sav beside the ROM under exactly the right
filename before the tool has anything to show — the fiddliest step of the whole
setup on a phone, and getting it wrong dead-ends on "Cannot read this save".
Fusing the save into the image removes the step: the .gba works on its own.

PRECEDENCE (the safety rule — see source/fused_sav.h)
-----------------------------------------------------
The fused save is a SEED, never an authority. At run time the flash chip wins
whenever it holds a valid Gen-3 save, because that is where the user's edits
live. The fused copy is read ONLY when flash is blank or unparseable, and it is
never programmed to the chip on boot. So fusing a save can never overwrite a
real one.

Corollary worth saying out loud: the fused copy does NOT update itself. Once
the user saves in-app, flash is the live save and this payload is a stale
snapshot. Re-fuse if you want the baked-in copy refreshed.

THE LOCATOR RECORD (16 bytes, 4-byte aligned, defined by the C side)

    offset  size  field
    +0       8    magic  : ASCII "PDNASAV1", no NUL terminator
    +8       4    offset : u32 little-endian, byte offset of the appended save
                           from the START of the fused file
    +12      4    size   : u32 little-endian, byte length of the appended save

    python3 tools/fuse_sav.py <pokedna.gba> <pokemon.sav> -o <out.gba>
    python3 tools/fuse_sav.py --check <out.gba>

ORDERING WITH fuse_rom.py
-------------------------
Both tools append to the end of the image and patch their own record, so they
compose — but run fuse_rom.py FIRST and fuse_sav.py second. fuse_rom.py --force
drops a previously appended payload by truncating, which would take a save
fused after it along with it. This tool refuses to run on an image whose ROM
record points past its own payload, so a wrong order is caught rather than
silently producing a broken image.

PRIVACY
-------
A save file is the user's own play data — it carries none of fuse_rom.py's
copyright weight. It is still personal: a fused image contains one person's
save, including their trainer name and ID. Don't publish one.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

MAGIC = b"PDNASAV1"                 # 8 bytes, no NUL
ROM_MAGIC = b"PDNAFUSE"             # fuse_rom.py's record, for the ordering check
RECORD_SIZE = 16                    # magic(8) + offset(4) + size(4)
ALIGN = 256                         # appended save starts on a 256-byte boundary
CART_WINDOW = 0x02000000            # 32 MiB — the GBA's cart address space

SAVE_SIZE = 0x20000                 # 128 KiB — G3_SAVE_FILE_SIZE
SAVE_SIZE_ALT = 0x10000             # 64 KiB dumps exist; the tool accepts and pads them

# --gb: a Game Boy battery save (source/gen2_save.h's G2_SAVE_SIZE/G2_MAX_RTC_TAIL).
# 32 KiB flat, or 32 KiB + a 44/48-byte MBC3 RTC clock footer some emulators append
# (bgb.bircd.org/rtcsave.html). source/pdna_gen12.c's pdna_gen12_size_is_gb() accepts
# the same window; keep the two in step.
GB_SAVE_SIZE = 0x8000                # 32768
GB_MAX_RTC_TAIL = 48

# --clip: an 80-byte Gen-3 PC box record, the THIRD optional payload (own locator,
# source/fused_sav.h's PdnaClipRec / magic PDNACLP1). Screenshot-only: seeds g_clip so
# an empty GB cell's mon-menu offers PASTE (GB) with nothing typed in by hand.
CLIP_MAGIC = b"PDNACLP1"
CLIP_SIZE = 80


def find_records(blob: bytes, magic: bytes) -> list[int]:
    """Every offset at which `magic` occurs (any alignment)."""
    hits, start = [], 0
    while True:
        i = blob.find(magic, start)
        if i < 0:
            return hits
        hits.append(i)
        start = i + 1


def locate_record(blob: bytes, magic: bytes, what: str) -> int:
    hits = find_records(blob, magic)
    if not hits:
        sys.exit(
            f"{what}: no {magic.decode()} locator record found.\n"
            "  This build predates fused-save support, or the record was\n"
            "  optimised out. The C side must declare it\n"
            "  __attribute__((used, aligned(4))) and const volatile.")
    if len(hits) > 1:
        listing = ", ".join(f"0x{h:X}" for h in hits)
        sys.exit(
            f"{what}: {len(hits)} {magic.decode()} records found at {listing}.\n"
            "  The magic must appear exactly once; a duplicate means the\n"
            "  literal got pooled. Build it element-by-element in C.")
    off = hits[0]
    if off % 4:
        sys.exit(
            f"{what}: locator record at 0x{off:X} is not 4-byte aligned.\n"
            "  The C side declares it aligned(4); a misaligned hit means this\n"
            "  is a coincidental byte sequence, not the record.")
    return off


def read_record(blob: bytes, rec_off: int) -> tuple[int, int]:
    return struct.unpack_from("<II", blob, rec_off + 8)


def write_record(buf: bytearray, rec_off: int, off: int, size: int) -> None:
    struct.pack_into("<II", buf, rec_off + 8, off, size)


def check_rom_payload_intact(blob: bytes) -> None:
    """If a ROM was fused first, make sure it still lies inside the file."""
    if ROM_MAGIC not in blob:
        return
    try:
        rec = locate_record(blob, ROM_MAGIC, "input (ROM record)")
    except SystemExit:
        return                       # not our problem to diagnose here
    off, size = read_record(blob, rec)
    if size and off + size > len(blob):
        sys.exit(
            f"input: the fused ROM record points at 0x{off:X}..0x{off + size:X}, "
            f"past the end of the file (0x{len(blob):X}).\n"
            "  The image looks truncated — did fuse_rom.py --force run AFTER a\n"
            "  save was fused? Re-fuse the ROM first, then the save.")


def fuse(pokedna_path: str, sav_path: str, out_path: str, force: bool,
          gb: bool = False, clip_path: str | None = None) -> int:
    with open(pokedna_path, "rb") as fh:
        base = bytearray(fh.read())
    with open(sav_path, "rb") as fh:
        sav = fh.read()

    if gb:
        # A Game Boy battery save (source/gen2_save.h G2_SAVE_SIZE/G2_MAX_RTC_TAIL,
        # source/pdna_gen12.c pdna_gen12_size_is_gb): 32 KiB flat, or 32 KiB plus a
        # 44/48-byte MBC3 RTC clock footer some emulators append. Never padded — the
        # C side accepts the whole window as-is.
        if not (GB_SAVE_SIZE <= len(sav) <= GB_SAVE_SIZE + GB_MAX_RTC_TAIL):
            sys.exit(
                f"{sav_path}: {len(sav)} bytes — expected {GB_SAVE_SIZE} (32 KiB) up "
                f"to {GB_SAVE_SIZE + GB_MAX_RTC_TAIL} (32 KiB + a 48-byte RTC tail).\n"
                "  --gb was passed but this is not a Game Boy save.")
    elif len(sav) not in (SAVE_SIZE, SAVE_SIZE_ALT):
        sys.exit(
            f"{sav_path}: {len(sav)} bytes — expected {SAVE_SIZE} (128 KiB) or "
            f"{SAVE_SIZE_ALT} (64 KiB).\n"
            "  That is not a Gen-3 .sav. Check you picked the save, not the ROM.\n"
            "  (Fusing a 32 KiB Game Boy save needs --gb.)")
    if not gb and len(sav) == SAVE_SIZE_ALT:
        # A 64 KiB dump has no sector 31; pad to the full image with 0xFF (erased
        # flash), which is exactly what gen3_parse already tolerates.
        print(f"  note         : 64 KiB dump padded to {SAVE_SIZE} with 0xFF")
        sav = sav + b"\xFF" * (SAVE_SIZE - SAVE_SIZE_ALT)

    if MAGIC in sav:
        sys.exit(
            f"{sav_path} itself contains the bytes {MAGIC.decode()} at "
            f"0x{sav.find(MAGIC):X}.\n"
            "  Fusing it would create a second copy of the magic and make the\n"
            "  record unlocatable. This should be effectively impossible for a\n"
            "  real save; check the file.")

    check_rom_payload_intact(bytes(base))
    rec_off = locate_record(bytes(base), MAGIC, pokedna_path)
    cur_off, cur_size = read_record(bytes(base), rec_off)
    if cur_size and not force:
        sys.exit(
            f"{pokedna_path} already has a save fused at 0x{cur_off:X} "
            f"({cur_size} bytes).\n"
            "  Re-fusing would append a second copy. Pass --force to replace it,\n"
            "  or fuse into a fresh build.")
    if cur_size and force:
        if cur_off + cur_size == len(base):
            print(f"  --force      : dropping the previous {cur_size}-byte payload")
            del base[cur_off:]
        else:
            sys.exit(
                "--force: the existing save payload is not at the end of the file, "
                "so dropping it would corrupt whatever follows.\n"
                "  Re-fuse from a clean build instead.")

    pad = (-len(base)) % ALIGN
    base.extend(b"\xFF" * pad)                  # 0xFF = erased-flash value
    off = len(base)
    base.extend(sav)

    if off + len(sav) > CART_WINDOW:
        sys.exit(
            f"fused image would be 0x{off + len(sav):X} bytes, past the "
            f"0x{CART_WINDOW:X} cartridge window.")

    write_record(base, rec_off, off, len(sav))

    with open(out_path, "wb") as fh:
        fh.write(base)

    # Verify by re-reading, the same way fuse_rom.py does — never trust the buffer.
    with open(out_path, "rb") as fh:
        back = fh.read()
    v_off, v_size = read_record(back, locate_record(back, MAGIC, out_path))
    ok = (v_off == off and v_size == len(sav)
          and back[v_off:v_off + v_size] == sav
          and all(b == 0xFF for b in back[off - pad:off]))

    print(f"  base         : {pokedna_path} ({len(base) - len(sav) - pad:,} bytes)")
    print(f"  save         : {sav_path} ({len(sav):,} bytes)")
    print(f"  record at    : 0x{rec_off:X}")
    print(f"  padding      : {pad} bytes of 0xFF")
    print(f"  save offset  : 0x{off:08X} ({off:,})")
    print(f"  cart address : 0x{0x08000000 + off:08X}")
    print(f"  output       : {out_path} ({len(back):,} bytes)")
    if not ok:
        sys.exit("  VERIFY: FAILED — output does not read back as written.")
    print("  VERIFY: OK — record patched and appended bytes match byte-for-byte")
    print("  NOTE: the fused save is a SEED. The flash chip wins whenever it holds a")
    print("        valid save, so this copy is only used when flash is blank, and it")
    print("        does NOT update when you save in-app. Re-fuse to refresh it.")
    print("  REMINDER: this file embeds a personal save (trainer name + IDs). Don't publish it.")

    if clip_path:
        fuse_clip(out_path, clip_path)
    return 0


def fuse_clip(out_path: str, clip_path: str) -> None:
    """Append an 80-byte Gen-3 box record as a THIRD payload (its own PDNACLP1
    locator) and patch it in place. Delta-only test hook (docs the D1 handoff): seeds
    g_clip at boot so PASTE (GB) is reachable on an empty cell without a Gen-3 session
    ever having been open. Runs on the file fuse() just wrote, after its own
    save-payload verification, so a clip failure never lands with a half-verified
    image."""
    with open(clip_path, "rb") as fh:
        clip = fh.read()
    if len(clip) != CLIP_SIZE:
        sys.exit(
            f"{clip_path}: {len(clip)} bytes — expected exactly {CLIP_SIZE} (one raw "
            "Gen-3 box record). tools/extract_gen3_record.c pulls one out of a real "
            ".sav.")

    with open(out_path, "rb") as fh:
        base = bytearray(fh.read())
    if CLIP_MAGIC in clip:
        sys.exit(f"{clip_path} itself contains the bytes {CLIP_MAGIC.decode()} — "
                 "pick a different slot, this one is unlocatable once fused.")

    rec_off = locate_record(bytes(base), CLIP_MAGIC, out_path)
    cur_off, cur_size = read_record(bytes(base), rec_off)
    if cur_size:
        if cur_off + cur_size == len(base):
            print(f"  --clip       : dropping the previous {cur_size}-byte payload")
            del base[cur_off:]
        else:
            sys.exit(
                "--clip: the existing clip payload is not at the end of the file, so "
                "dropping it would corrupt whatever follows.\n"
                "  Re-fuse from a clean build instead.")

    pad = (-len(base)) % ALIGN
    base.extend(b"\xFF" * pad)
    off = len(base)
    base.extend(clip)
    if off + len(clip) > CART_WINDOW:
        sys.exit(f"fused image would be 0x{off + len(clip):X} bytes, past the "
                 f"0x{CART_WINDOW:X} cartridge window.")
    write_record(base, rec_off, off, len(clip))

    with open(out_path, "wb") as fh:
        fh.write(base)

    with open(out_path, "rb") as fh:
        back = fh.read()
    v_off, v_size = read_record(back, locate_record(back, CLIP_MAGIC, out_path))
    ok = (v_off == off and v_size == len(clip)
          and back[v_off:v_off + v_size] == clip
          and all(b == 0xFF for b in back[off - pad:off]))

    print(f"  clip         : {clip_path} ({len(clip)} bytes)")
    print(f"  clip record  : 0x{rec_off:X}")
    print(f"  clip offset  : 0x{off:08X} ({off:,})")
    print(f"  output       : {out_path} ({len(back):,} bytes)")
    if not ok:
        sys.exit("  CLIP VERIFY: FAILED — output does not read back as written.")
    print("  CLIP VERIFY: OK — record patched and appended bytes match byte-for-byte")


def check_only(path: str) -> int:
    with open(path, "rb") as fh:
        blob = fh.read()
    rec = locate_record(blob, MAGIC, path)
    off, size = read_record(blob, rec)
    print(f"  file         : {path} ({len(blob):,} bytes)")
    print(f"  record at    : 0x{rec:X}")
    if not size:
        print("  fused save   : none (offset/size are 0)")
        return 0
    print(f"  save offset  : 0x{off:08X} ({off:,})")
    print(f"  save size    : 0x{size:X} ({size:,})")
    bad = []
    if off % ALIGN:
        bad.append(f"offset is not {ALIGN}-byte aligned")
    if off + size > len(blob):
        bad.append("payload runs past the end of the file")
    is_gb = GB_SAVE_SIZE <= size <= GB_SAVE_SIZE + GB_MAX_RTC_TAIL
    if size not in (SAVE_SIZE, SAVE_SIZE_ALT) and not is_gb:
        bad.append(f"size is not {SAVE_SIZE} or {SAVE_SIZE_ALT} (Gen-3), or a "
                    f"{GB_SAVE_SIZE}..{GB_SAVE_SIZE + GB_MAX_RTC_TAIL} GB size")
    if bad:
        print("  PROBLEMS: " + "; ".join(bad))
        return 1
    print("  looks consistent" + ("  (Game Boy save)" if is_gb else ""))

    if CLIP_MAGIC in blob:
        crec = locate_record(blob, CLIP_MAGIC, path)
        coff, csize = read_record(blob, crec)
        if not csize:
            print("  fused clip   : none (offset/size are 0)")
        else:
            cbad = []
            if coff % ALIGN:
                cbad.append("offset is not %d-byte aligned" % ALIGN)
            if coff + csize > len(blob):
                cbad.append("payload runs past the end of the file")
            if csize != CLIP_SIZE:
                cbad.append(f"size is not {CLIP_SIZE}")
            print(f"  clip offset  : 0x{coff:08X} ({coff:,})  clip size: {csize}")
            print("  clip PROBLEMS: " + "; ".join(cbad) if cbad else "  clip looks consistent")
            if cbad:
                return 1
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Append a Pokemon .sav onto a PokeDNA build so the emulator "
                    "build needs no separate save file.",
        epilog="The fused save seeds a blank flash chip only; it never overwrites "
               "a real save, and it does not update when you save in-app.")
    ap.add_argument("pokedna", nargs="?", help="the PokeDNA .gba build")
    ap.add_argument("sav", nargs="?", help="the Pokemon .sav to append")
    ap.add_argument("-o", "--output", help="fused output .gba")
    ap.add_argument("--check", metavar="FUSED.GBA",
                    help="just report the save locator record of an image")
    ap.add_argument("--force", action="store_true",
                    help="replace an already-fused save (drops the old payload). If a "
                         "--clip record was fused AFTER it, this aborts by design "
                         "(dropping the save would corrupt the clip payload sitting "
                         "past it) — re-fuse both from a clean build instead")
    ap.add_argument("--gb", action="store_true",
                    help="the save is a Game Boy (Gen-1/2) battery image, not Gen-3 — "
                         "32 KiB, or 32 KiB + a 44/48-byte RTC tail")
    ap.add_argument("--clip", metavar="REC80.BIN",
                    help="delta-only test hook: also fuse an 80-byte raw Gen-3 box "
                         "record (own locator) so PDNA_DELTA boot pre-fills g_clip, "
                         "making PASTE (GB) on an empty cell reachable without a "
                         "Gen-3 session ever having been open. "
                         "tools/extract_gen3_record.c pulls one out of a real .sav")
    a = ap.parse_args(argv)

    if a.check:
        return check_only(a.check)
    if not (a.pokedna and a.sav and a.output):
        ap.error("need <pokedna.gba> <pokemon.sav> -o <out.gba>, or --check FILE")
    for p in (a.pokedna, a.sav):
        if not os.path.isfile(p):
            sys.exit(f"{p}: not a file")
    if a.clip and not os.path.isfile(a.clip):
        sys.exit(f"{a.clip}: not a file")
    return fuse(a.pokedna, a.sav, a.output, a.force, gb=a.gb, clip_path=a.clip)


if __name__ == "__main__":
    sys.exit(main())
