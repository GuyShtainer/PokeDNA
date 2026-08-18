#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""stamp_rom_windows.py - post-link: teach PokeDNA.gba what its own bytes look like.

    python3 tools/stamp_rom_windows.py --elf PokeDNA.elf --gba PokeDNA.gba
    python3 tools/stamp_rom_windows.py --verify /Volumes/EZFLASH/PokeDNA.gba

The ROM carries a descriptor (source/pdna_romver_data.c, symbol `g_pdna_romver`)
listing sampled windows as (anchor symbol address, offset, length). This tool finds
that descriptor BY SYMBOL, computes each window's CRC32 over the FINAL .gba, and
patches the CRCs in. At boot the cartridge re-reads those windows and says
"ROM IMAGE MODIFIED OR INCOMPLETE - re-copy me" instead of hanging.

WHY BY SYMBOL AND NOT BY MAGIC SCAN
    source/pdna_romver.c's own literal pool contains byte-identical copies of the two
    magic words. A scan can latch onto one; rom-load-lab hit exactly this and wrote the
    rule down (rom-load-lab/source/diag_data.c:6-10). --verify has no ELF to consult, so
    it scans and then demands SELF-REFERENCE: a real stamped descriptor records its own
    file offset in hole_off. A literal-pool copy cannot.

    Note what --verify's acceptance test does NOT include: image_bytes == filesize. That
    would make a truncated file yield "no descriptor found" instead of reaching the SIZE
    MISMATCH report - and "is the copy on my card short?" is the single most important
    question this tool answers.

THE HOLE RULE
    The descriptor's own 624 bytes are the hole. crc_whole is computed with the hole read
    as zeroes, so re-stamping the same image is bit-for-bit idempotent, and no window may
    overlap the hole (its CRC would be self-invalidating). rom-load-lab/source/diag.h:96-111.

WHAT THE EZ-FLASH KERNEL DOES TO THE IMAGE, AND WHY THE GUARDS BELOW EXIST
    On an SD load the Omega DE kernel patches the copy in PSRAM
    (reference/omega-de-kernel/source/GBApatch.c: GBApatch_PSRAM -> Patch_Reset_Sleep ->
    Patch_B_address):
      * ROM word 0 is rewritten to branch to injected code    -> HEAD_GUARD
      * a sleep/RTS blob is written near EOF at iTrimSize      -> TAIL_GUARD
      * every aligned 0x03007FFC / 0x03FFFFFC word becomes
        0x03007FF4 (PatchInternal, cached in /PATCH)           -> IRQ word scan
    A window on any of those would fail on EVERY SD boot, so this tool refuses to build
    one, loudly, naming the offset to move.

    That is also why crc_whole is stamped for THIS tool's use only: the running image
    legitimately is not the file, so the cartridge must never check it.

THE THUMB BIT
    An R_ARM_ABS32 against a Thumb function symbol carries the interworking bit, so an
    anchor that is a function lands in the descriptor ODD (measured on devkitARM 15.2.0:
    nm says 0800026c, the relocated word reads 0800026d). Both this tool and
    pdna_rv_check mask bit 0. The stored value is left exactly as the linker wrote it.
"""
from __future__ import annotations

import argparse
import bisect
import json
import struct
import subprocess
import sys
import zlib

# The Makefile runs this inside devkitpro/devkitarm too, so the floor is declared here
# rather than discovered as a SyntaxError that deletes the .gba mid-build.
if sys.version_info < (3, 8):
    sys.stderr.write("*** FATAL: stamp_rom_windows.py needs Python 3.8 or newer "
                     "(found %d.%d)\n" % sys.version_info[:2])
    sys.exit(1)

ROM_BASE = 0x08000000
DESC_SYM = "g_pdna_romver"
FUSE_SYM = "g_pdna_fuse"        # tools/fuse_rom.py patches these 16 bytes post-link
FUSE_BYTES = 16

MAGIC0 = 0x414E4450             # 'PDNA'
MAGIC1 = 0x31305652             # 'RV01'
STAMPED = 0x504D5453            # 'STMP'
VERSION = 1

HDR_BYTES = 48
WIN_BYTES = 24
MAX_WINDOWS = 24
DESC_BYTES = HDR_BYTES + MAX_WINDOWS * WIN_BYTES        # 624

HEAD_GUARD = 0x100              # the kernel rewrites ROM[0]
TAIL_GUARD = 0x10000            # the kernel drops its patch blob near EOF (iTrimSize)
MAX_WINLEN = 65536              # must match PDNA_RV_MAX_WINLEN
IRQ_WORDS = (0x03007FFC, 0x03FFFFFC)

# window flags - must match source/pdna_romver.h
F_CONTROL = 0x0001
F_EXACT = 0x0002
F_K_TEXT = 0x0010
F_K_TABLE = 0x0020
F_K_ART = 0x0040

# field order must match PdnaRomVerify in source/pdna_romver.h
HDR_FMT = "<12I"    # m0 m1 ver stamped image n_win n_present hole_off hole_len whole r0 r1
WIN_FMT = "<5I4s"   # base off len crc flags tag


def die(msg, *rest):
    sys.stderr.write("*** FATAL: %s\n" % msg)
    for r in rest:
        sys.stderr.write("***        %s\n" % r)
    sys.exit(1)


def kind_of(flags):
    if flags & F_K_TEXT:
        return "ctrl" if flags & F_CONTROL else "code"
    if flags & F_K_TABLE:
        return "table"
    if flags & F_K_ART:
        return "art"
    return "?"


# ---------------------------------------------------------------- nm ------------
def nm_symbols(nm, elf, rom_end):
    """-> (name->addr, sorted list of distinct ROM-range addresses)"""
    try:
        p = subprocess.run([nm, "-n", elf], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, universal_newlines=True)
    except OSError as e:
        die("could not run %s: %s" % (nm, e))
    if p.returncode != 0:
        die("%s -n %s failed" % (nm, elf), p.stderr.strip())
    byname = {}
    addrs = set()
    for line in p.stdout.splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue                      # undefined symbols: "         U foo"
        try:
            a = int(parts[0], 16)
        except ValueError:
            continue
        byname.setdefault(parts[2], a)
        if ROM_BASE <= a <= rom_end:
            addrs.add(a)
    return byname, sorted(addrs)


def extent_end(addrs, base, rom_end):
    """First symbol strictly above `base`: where the anchor's own data ends."""
    i = bisect.bisect_right(addrs, base)
    return addrs[i] if i < len(addrs) else rom_end


# ------------------------------------------------------------- descriptor -------
def read_hdr(data, off):
    f = struct.unpack_from(HDR_FMT, data, off)
    return dict(magic0=f[0], magic1=f[1], version=f[2], stamped=f[3],
                image_bytes=f[4], n_windows=f[5], n_present=f[6],
                hole_off=f[7], hole_len=f[8], crc_whole=f[9])


def read_win(data, off, i):
    base, o, ln, crc, flags, tag = struct.unpack_from(
        WIN_FMT, data, off + HDR_BYTES + i * WIN_BYTES)
    return dict(base=base, off=o, len=ln, crc=crc, flags=flags,
                tag=tag.decode("latin-1"))


def win_field_off(desc_off, i, field):
    """Byte offset of one u32 field of window i. field: 0=base 1=off 2=len 3=crc."""
    return desc_off + HDR_BYTES + i * WIN_BYTES + 4 * field


def hole_zeroed(data, off):
    b = bytearray(data)
    b[off:off + DESC_BYTES] = b"\0" * DESC_BYTES
    return bytes(b)


def scan_for_desc(data):
    """--verify has no ELF. A magic scan alone is unsafe, so a candidate only counts when
    it says WHERE IT IS: hole_off == its own offset. Deliberately no size predicate - see
    the module docstring."""
    needle = struct.pack("<II", MAGIC0, MAGIC1)
    hits = []
    start = 0
    while True:
        i = data.find(needle, start)
        if i < 0:
            break
        start = i + 4
        if i % 4 or i + DESC_BYTES > len(data):
            continue
        h = read_hdr(data, i)
        if (h["stamped"] == STAMPED and h["version"] == VERSION
                and h["hole_off"] == i and h["hole_len"] == DESC_BYTES
                and h["n_windows"] <= MAX_WINDOWS):
            hits.append(i)
    if not hits:
        die("no stamped PokeDNA self-check descriptor in this file",
            "Either it was built without python3, or this is not a stamped PokeDNA.gba.")
    if len(hits) > 1:
        die("%d self-consistent descriptors found - refusing to guess" % len(hits),
            "Pass --desc-off 0x... to disambiguate.")
    return hits[0]


# ----------------------------------------------------------------- stamp --------
def stamp(elf, gba, nm, quiet, manifest):
    with open(gba, "rb") as fh:
        data = bytearray(fh.read())
    size = len(data)
    rom_end = ROM_BASE + size

    byname, addrs = nm_symbols(nm, elf, rom_end)
    addrset = set(addrs)
    if DESC_SYM not in byname:
        die("symbol %s not found in %s" % (DESC_SYM, elf),
            "source/pdna_romver_data.c must be compiled in, and pdna_romcheck.c must",
            "reference it - gba.specs links with --gc-sections, so an unreferenced",
            "descriptor is silently dropped.")
    desc_addr = byname[DESC_SYM]
    desc_off = desc_addr - ROM_BASE
    if desc_off < 0 or desc_off + DESC_BYTES > size:
        die("%s at 0x%08x is outside the %d-byte image" % (DESC_SYM, desc_addr, size))

    h = read_hdr(data, desc_off)
    if h["magic0"] != MAGIC0 or h["magic1"] != MAGIC1:
        die("%s does not start with the magic" % DESC_SYM,
            "got %08x %08x - pdna_romver.h and this tool disagree."
            % (h["magic0"], h["magic1"]))
    if h["version"] != VERSION:
        die("descriptor version %d != %d - update this tool" % (h["version"], VERSION))
    n_windows = h["n_windows"]
    if n_windows > MAX_WINDOWS:
        die("n_windows=%d > PDNA_RV_MAX_WINDOWS=%d" % (n_windows, MAX_WINDOWS))

    hole0, hole1 = desc_off, desc_off + DESC_BYTES
    fuse0 = fuse1 = -1
    if FUSE_SYM in byname:
        fuse0 = byname[FUSE_SYM] - ROM_BASE
        fuse1 = fuse0 + FUSE_BYTES

    present = 0
    absent = 0
    report = []
    manifest_windows = []

    for i in range(MAX_WINDOWS):
        w = read_win(data, desc_off, i)
        tag = w["tag"]

        if i >= n_windows:
            if w["base"] or w["off"] or w["len"]:
                die("window %d is beyond n_windows=%d but is not empty" % (i, n_windows),
                    "You added a table entry and forgot to bump .n_windows in",
                    "source/pdna_romver_data.c.")
            continue

        # ANCHOR-ABSENT COMES FIRST, before any complaint about len. An artless build and
        # `make sd` legitimately link without some anchors, and after the first stamp
        # their len is already 0 - so checking len first would make re-stamping such an
        # image die blaming the source table. (base is masked: a Thumb function anchor
        # arrives with bit 0 set.)
        base = w["base"] & ~1
        if base == 0:
            struct.pack_into("<I", data, win_field_off(desc_off, i, 2), 0)
            struct.pack_into("<I", data, win_field_off(desc_off, i, 3), 0)
            report.append("    %4s  absent in this build" % tag)
            absent += 1
            continue

        ln = w["len"]
        if ln == 0:
            die("window %d (%r) has a present anchor but len 0 in the source table"
                % (i, tag))
        if ln > MAX_WINLEN:
            die("window %d (%s) len %d exceeds PDNA_RV_MAX_WINLEN=%d"
                % (i, tag, ln, MAX_WINLEN),
                "The runtime clamps to that and would SKIP this window.")

        off = w["off"]
        start, end = base + off, base + off + ln
        s_off, e_off = start - ROM_BASE, end - ROM_BASE

        if (base | off | ln) & 3:
            die("window %d (%s) is not 4-aligned: base=0x%08x off=0x%x len=%d"
                % (i, tag, base, off, ln),
                "The cartridge reads windows 32 bits at a time; keep everything 4-aligned.")
        if base not in addrset:
            die("window %d (%s) anchor 0x%08x is not a symbol address" % (i, tag, base),
                "The relocation did not land on a symbol - the table is corrupt.")
        if e_off > size:
            die("window %d (%s) ends at file 0x%x, past the %d-byte image"
                % (i, tag, e_off, size))
        if s_off < HEAD_GUARD:
            die("window %d (%s) starts at file 0x%x, inside the first 0x%x bytes"
                % (i, tag, s_off, HEAD_GUARD),
                "The EZ-Flash kernel rewrites ROM word 0 (Patch_B_address).")
        if e_off > size - TAIL_GUARD:
            die("window %d (%s) ends at file 0x%x, inside the last %d KiB"
                % (i, tag, e_off, TAIL_GUARD // 1024),
                "The EZ-Flash kernel writes its sleep/RTS patch blob there (iTrimSize).")
        if s_off < hole1 and e_off > hole0:
            die("window %d (%s) overlaps the descriptor hole [0x%x,0x%x)"
                % (i, tag, hole0, hole1),
                "A window may not checksum the bytes it is stored in (the hole rule).")
        if fuse0 >= 0 and s_off < fuse1 and e_off > fuse0:
            die("window %d (%s) overlaps the %s record [0x%x,0x%x)"
                % (i, tag, FUSE_SYM, fuse0, fuse1),
                "tools/fuse_rom.py patches those 16 bytes post-link, which would make",
                "every fused test image report a false ROM-corruption alarm.")
        if w["flags"] & F_EXACT:
            lim = extent_end(addrs, base, rom_end)
            if end > lim:
                die("window %d (%s) runs past its anchor: ends 0x%08x, next symbol 0x%08x"
                    % (i, tag, end, lim),
                    "The blob shrank. Largest usable off for len=%d is 0x%x."
                    % (ln, (lim - ln - base) & ~3))

        seg = bytes(data[s_off:e_off])
        for k in range(0, ln, 4):
            word = struct.unpack_from("<I", seg, k)[0]
            if word in IRQ_WORDS:
                die("window %d (%s) contains 0x%08X at file 0x%x"
                    % (i, tag, word, s_off + k),
                    "The EZ-Flash kernel rewrites that word to 0x03007FF4 in PSRAM, so",
                    "this window would fail on every SD boot. Nudge `off` in",
                    "source/pdna_romver_data.c by 4 KiB and rebuild.")
        if len(set(seg)) == 1:
            die("window %d (%s) is a single repeated byte 0x%02x" % (i, tag, seg[0]),
                "A constant window cannot tell a good load from a zero fill. Move `off`.")

        crc = zlib.crc32(seg) & 0xFFFFFFFF
        struct.pack_into("<I", data, win_field_off(desc_off, i, 2), ln)
        struct.pack_into("<I", data, win_field_off(desc_off, i, 3), crc)
        present += 1
        report.append("    %4s  0x%08x (%5.2f MB)  %5d B  crc=%08x  %s"
                      % (tag, s_off, s_off / 1e6, ln, crc, kind_of(w["flags"])))
        manifest_windows.append(dict(tag=tag, file_off=s_off, len=ln,
                                     kind=kind_of(w["flags"]), crc="%08x" % crc))

    crc_whole = zlib.crc32(hole_zeroed(bytes(data), desc_off)) & 0xFFFFFFFF
    struct.pack_into(HDR_FMT, data, desc_off,
                     MAGIC0, MAGIC1, VERSION, STAMPED, size,
                     n_windows, present, desc_off, DESC_BYTES, crc_whole, 0, 0)

    # Idempotency self-check: stamping must not change anything the CRCs cover.
    again = zlib.crc32(hole_zeroed(bytes(data), desc_off)) & 0xFFFFFFFF
    if again != crc_whole:
        die("internal: stamping changed bytes outside the hole (not idempotent)")

    with open(gba, "wb") as fh:
        fh.write(bytes(data))
    if manifest:
        with open(manifest, "w") as fh:
            fh.write(json.dumps(dict(gba=gba, image_bytes=size, desc_off=desc_off,
                                     crc_whole="%08x" % crc_whole,
                                     windows=manifest_windows), indent=2) + "\n")

    sampled = sum(x["len"] for x in manifest_windows)
    if not quiet:
        for line in report:
            print(line)
    pct = (sampled / size * 100.0) if size else 0.0
    print("  ROM self-check: %d/%d windows stamped (%d absent), %d B sampled "
          "(%.3f%% of image), whole=0x%08x"
          % (present, n_windows, absent, sampled, pct, crc_whole))
    if present == 0:
        print("  ROM self-check: no anchors in this build - verification is a no-op")


# ---------------------------------------------------------------- verify --------
def verify(gba, desc_off):
    with open(gba, "rb") as fh:
        data = fh.read()
    off = desc_off if desc_off is not None else scan_for_desc(data)
    h = read_hdr(data, off)
    print("  descriptor at 0x%x: %d/%d windows, stamped at %d B"
          % (off, h["n_present"], h["n_windows"], h["image_bytes"]))
    if h["image_bytes"] != len(data):
        delta = len(data) - h["image_bytes"]
        what = "TRUNCATED" if delta < 0 else "PADDED OR APPENDED TO"
        print("  SIZE MISMATCH: file is %d B, was stamped at %d B (%+d) -> %s"
              % (len(data), h["image_bytes"], delta, what))
        if delta > 0:
            print("  (a fused test image from tools/fuse_rom.py looks exactly like this)")
        return 2

    bad = 0
    for i in range(h["n_windows"]):
        w = read_win(data, off, i)
        base = w["base"] & ~1
        if base == 0 or w["len"] == 0:
            print("    %4s  absent in this build" % w["tag"])
            continue
        s = base + w["off"] - ROM_BASE
        got = zlib.crc32(data[s:s + w["len"]]) & 0xFFFFFFFF
        ok = got == w["crc"]
        detail = "" if ok else "  exp=%08x got=%08x" % (w["crc"], got)
        bad += 0 if ok else 1
        print("    %4s  0x%08x  %-5s %-5s%s"
              % (w["tag"], s, kind_of(w["flags"]), "ok" if ok else "BAD", detail))

    whole = zlib.crc32(hole_zeroed(data, off)) & 0xFFFFFFFF
    wok = whole == h["crc_whole"]
    wdetail = "ok" if wok else "BAD (exp 0x%08x)" % h["crc_whole"]
    print("    WHOLE 0x%08x %s" % (whole, wdetail))
    if bad or not wok:
        print("  VERDICT: FILE CORRUPT (%d sampled window(s) bad, whole %s) - "
              "the copy failed, not the load." % (bad, "ok" if wok else "bad"))
        return 2
    print("  VERDICT: file matches its own stamp, byte for byte.")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--elf", help="the linked .elf (source of the descriptor's address)")
    ap.add_argument("--gba", help="the final .gba to stamp IN PLACE")
    ap.add_argument("--nm", default="arm-none-eabi-nm")
    ap.add_argument("--quiet", action="store_true", help="summary line only")
    ap.add_argument("--manifest", metavar="JSON",
                    help="also write the window table as JSON (off by default)")
    ap.add_argument("--verify", metavar="GBA",
                    help="check a .gba against its own stamp; no ELF needed")
    ap.add_argument("--desc-off", type=lambda s: int(s, 0),
                    help="skip the descriptor scan (--verify only)")
    a = ap.parse_args()
    if a.verify:
        return verify(a.verify, a.desc_off)
    if not (a.elf and a.gba):
        ap.error("--elf and --gba are required (or use --verify)")
    stamp(a.elf, a.gba, a.nm, a.quiet, a.manifest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
