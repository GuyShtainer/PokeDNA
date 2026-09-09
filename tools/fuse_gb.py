#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""fuse_gb.py — append N Game Boy / Game Boy Color payloads (ROMs and/or battery
saves) onto a PokeDNA build, with ONE directory block describing all of them.

WHY THIS EXISTS
---------------
fuse_rom.py fuses exactly one Gen-3 ROM; fuse_sav.py fuses exactly one Gen-3 (or
one Gen-1/2) save. BACKLOG #62 wants a "delta" build for Guy's iPhone (Delta
emulator) that carries a WHOLE Game Boy corpus at once — Red.gb + Red.sav,
Gold.gbc + Gold.sav, Crystal.gbc + Crystal.sav — so the GB half of PokeDNA (box
art, menu icons, CREATE's ROM-derived stats/moveset, the GB save picker) has
real data to read under an emulator that has no flashcart and no SD card. One
fixed-shape record per payload would mean a new tool (or a pile of optional
flags) every time the corpus grows; a small typed directory does not.

THE DIRECTORY (found via the SAME locator-record convention as fuse_rom.py /
fuse_sav.py — a unique magic string, declared `const volatile` on the C side so
it survives --gc-sections and cannot be constant-folded away)

    g_pdna_gbd (source/fused_gb.h/.c): magic "PDNAGBD1" + u32 offset + u32 size,
    exactly the same 16-byte shape as PdnaFuseRec/PdnaSavRec. `offset`/`size`
    point at the DIRECTORY BLOCK below (not at any one payload) — the app reads
    the block once at the recorded offset/size and finds every payload inside.

    directory block layout (all fields little-endian, 4-byte aligned):

        offset  size  field
        +0       8    magic  : ASCII "PDNAGBD1" (redundant with the locator's own
                                magic — a second independent check that the
                                block itself was not truncated/corrupted)
        +8       4    count  : number of entries
        +12    48*N   entries[count], each:
                          +0   4   type   : 1=ROM_GEN1  2=ROM_GEN2  3=SAV
                          +4  32   name   : ASCII, NUL-padded (e.g. "Red.gb")
                          +36  4   offset : u32, byte offset from the START of
                                            the fused FILE (not from the
                                            directory) — same convention as
                                            PdnaFuseRec/PdnaSavRec so the app's
                                            existing "CART_BASE + offset" memcpy
                                            reader needs no directory-relative
                                            math
                          +40  4   size   : u32, payload length in bytes
                          +44  4   crc32  : zlib CRC-32 of the payload bytes
        +12+48*N 4    dir_size : u32, total directory-block length INCLUDING
                                  this trailer (i.e. 12 + 48*count + 16) — lets
                                  the app sanity-check its own record's `size`
                                  field before trusting `count`
        +16+48*N 8    magic (again) : ASCII "PDNAGBD1"
        +24+48*N 4    reserved (0)

    The trailing 16 bytes (dir_size + magic + reserved) are ALSO, deliberately,
    exactly the last 16 bytes of the whole fused image (payloads are appended
    strictly before the directory, and the directory is the very last thing in
    the file) — so a caller who does not want to trust the locator record for
    some reason can still find the directory by reading EOF-16, matching this
    tool's own header comment about "the app finds it by reading the last 16
    bytes of the cart image". PokeDNA's own C side uses the locator record
    (cheap, matches the other two tools, no runtime "how big is the ROM"
    problem — see source/fused_gb.h for why EOF-scanning was rejected).

WHERE THE PAYLOADS THEMSELVES LIVE
-----------------------------------
Appended back-to-back, each individually 4-byte aligned (ALIGN below), in the
order given on the command line, BEFORE the directory block. Order does not
matter to the reader — every payload is located by its own directory entry.

USAGE
-----
    python3 tools/fuse_gb.py <pokedna.gba> PAY [PAY ...] -o <out.gba>
    python3 tools/fuse_gb.py --check <fused.gba>

PAY is a path whose extension selects its type: .gb -> ROM_GEN1, .gbc ->
ROM_GEN2, .sav -> SAV. Run fuse_sav.py FIRST (the Gen-3 flash-save seed) and
this tool second — same ordering rule as fuse_rom.py/fuse_sav.py compose today
(each appends at the CURRENT end of file and refuses to re-fuse without
--force).

CAP
---
Refuses if the total fused image would exceed the GBA's 32 MiB cartridge
address window (0x08000000..0x09FFFFFF) — nothing past that offset is ever
reachable by any reader that does `0x08000000 + offset`.

LEGAL / PRIVACY
----------------
Every .gb/.gbc payload is a verbatim copy of a commercial Game Boy ROM — same
weight as fuse_rom.py's Gen-3 ROM. Every .sav is personal play data — same
weight as fuse_sav.py's save. NEVER commit, publish, or share the output.
"""

from __future__ import annotations

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

MAGIC = b"PDNAGBD1"                 # 8 bytes, no NUL — the directory's own magic
FUSE_MAGIC = b"PDNAFUSE"            # fuse_rom.py's own record -- used only to disambiguate
SAV_MAGIC = b"PDNASAV1"             # fuse_sav.py's own record -- same purpose
RECORD_SIZE = 16                    # the g_pdna_gbd locator record: magic+offset+size
ENTRY_SIZE = 48                     # type(4) + name(32) + offset(4) + size(4) + crc32(4)
NAME_SIZE = 32
TRAILER_SIZE = 16                   # dir_size(4) + magic(8) + reserved(4)
ALIGN = 256                         # every payload (and the directory) starts aligned
CART_WINDOW = 0x02000000            # 32 MiB — the GBA cartridge address window
CART_BASE = 0x08000000

TYPE_ROM_GEN1 = 1
TYPE_ROM_GEN2 = 2
TYPE_SAV = 3
TYPE_LOC = 4                         # BACKLOG #68b: a fused rom_gb*_open_loc() record,
                                      # see LOC_HDR_* below for the payload's own header
TYPE_NAMES = {TYPE_ROM_GEN1: "ROM_GEN1", TYPE_ROM_GEN2: "ROM_GEN2", TYPE_SAV: "SAV",
              TYPE_LOC: "LOC"}

EXT_TYPE = {".gb": TYPE_ROM_GEN1, ".gbc": TYPE_ROM_GEN2, ".sav": TYPE_SAV}

# BACKLOG #68b: tools/gbloc_driver.c runs the SHIPPED rom_gbsprite.c/rom_gbicon.c/
# rom_gbui.c locators against a fused ROM at fuse time and writes one of these small
# payloads per locator the ROM satisfies. See gbloc_driver.c's own header comment for
# the exact byte layout; kept in sync here only for --check's independent re-verify.
LOC_HDR_MAGIC = b"PDNALOC1"
LOC_HDR_SIZE = 20                    # magic(8)+kind(1)+gen(1)+rec_size(2)+id_hash(4)+rom_size(4)
LOC_KIND_SPRITE = 1
LOC_KIND_ICON = 2
LOC_KIND_UI = 3
LOC_KIND_NAMES = {LOC_KIND_SPRITE: "sprite", LOC_KIND_ICON: "icon", LOC_KIND_UI: "ui"}


def fnv1a32(data: bytes, h: int = 0x811C9DC5) -> int:
    """The exact FNV-1a rom_gbsprite.c/rom_gbicon.c/rom_gbui.c each define locally
    (`fnv1a`, seed 0x811C9DC5, prime 0x01000193) -- reimplemented here ONLY so
    --check can independently recompute a ROM's id_hash from its own header bytes
    and cross-verify a fused LOC payload's claim, rather than trusting whatever
    gbloc_driver wrote about itself."""
    for b in data:
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def gb_id_hash(rom_bytes: bytes) -> int:
    """id_hash as every rom_gb*.c computes it: FNV-1a over the 0x100..0x14F header
    window (0x50 bytes)."""
    return fnv1a32(rom_bytes[0x100:0x150])


def build_gbloc_driver(bin_path: Path) -> None:
    cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-I", str(ROOT / "source"),
           str(ROOT / "tools" / "gbloc_driver.c"),
           str(ROOT / "source" / "rom_gbsprite.c"),
           str(ROOT / "source" / "rom_gbicon.c"),
           str(ROOT / "source" / "rom_gbui.c"),
           str(ROOT / "source" / "gb_sprite_codec.c"),
           "-o", str(bin_path)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise FuseError(f"failed to build tools/gbloc_driver.c:\n{r.stdout}\n{r.stderr}")


def gbloc_payloads(driver_bin: Path, rom_path: str) -> list[bytes]:
    """Runs the compiled gbloc_driver against `rom_path` and splits its combined
    output into individual [header+record] payload chunks, in the order the driver
    wrote them (sprite, then icon if Gen 2, then ui) -- see gbloc_driver.c's own
    header comment for the exact per-payload layout this parses."""
    with tempfile.NamedTemporaryFile(delete=False) as tmp:
        out_path = tmp.name
    try:
        r = subprocess.run([str(driver_bin), rom_path, out_path],
                           capture_output=True, text=True)
        if r.returncode != 0:
            # Not fatal: a ROM the shipped locators cannot place any table in just
            # gets no LOC entries -- the delta build scans it as it does today.
            print(f"  ! gbloc_driver could not locate anything in {rom_path} "
                  f"(rc={r.returncode}); no LOC payload fused for it.")
            return []
        blob = open(out_path, "rb").read()
    finally:
        os.unlink(out_path)

    chunks = []
    pos = 0
    while pos < len(blob):
        if blob[pos:pos + 8] != LOC_HDR_MAGIC:
            raise FuseError(f"gbloc_driver output for {rom_path} is malformed at byte {pos}")
        rec_size = struct.unpack_from("<H", blob, pos + 10)[0]
        total = LOC_HDR_SIZE + rec_size
        chunks.append(blob[pos:pos + total])
        pos += total
    if pos != len(blob):
        raise FuseError(f"gbloc_driver output for {rom_path} has a trailing partial payload")
    return chunks

# Valid GB/GBC ROM sizes: 32 KiB * 2^n, n=0..8 (32 KiB .. 8 MiB), per the cartridge
# header's own ROM-size byte encoding (0x148). Accepted range is generous on purpose —
# this tool does not need to validate a commercial dump's exact banking, only that it
# looks like a whole ROM and not something truncated.
MIN_ROM_SIZE = 0x8000
MAX_ROM_SIZE = 0x800000

# Save sizes: Gen-1 SRAM is up to 0x8000 (32 KiB, MBC1/3); Gen-2 is a fixed 0x8000
# (G2_SAVE_SIZE) optionally plus an MBC3 RTC tail up to 48 bytes some emulators/flash
# carts append (source/gen2_save.h G2_MAX_RTC_TAIL) — accept both, same window
# fuse_sav.py's --gb already validates against.
MIN_SAV_SIZE = 0x2000
MAX_SAV_TAIL = 48
MAX_SAV_SIZE = 0x8000 + MAX_SAV_TAIL


class FuseError(Exception):
    pass


def find_records(blob: bytes, magic: bytes) -> list[int]:
    hits, start = [], 0
    while True:
        i = blob.find(magic, start)
        if i < 0:
            return hits
        hits.append(i)
        start = i + 1


def locate_record(blob: bytes, what: str) -> int:
    hits = find_records(blob, MAGIC)
    if not hits:
        raise FuseError(
            f"{what}: no {MAGIC.decode()} locator record found.\n"
            "  This build predates fused-GB support (source/fused_gb.c must define\n"
            "  g_pdna_gbd, const volatile, __attribute__((used, aligned(4)))).")
    if len(hits) > 1:
        # #62 review D7: filter to (offset,size) == (0,0) candidates FIRST, before the
        # proximity heuristic below -- a never-fused g_pdna_gbd record is DEFINITIONALLY
        # magic+0+0 (the struct's own initializer), so this alone disambiguates the
        # documented collision (a shiny-sprite blob at BACKLOG #62's own real build
        # spelled PDNAGBD1 + a NONzero (0, 43459) pair right after it -- proximity to
        # the fuse_rom.py/fuse_sav.py anchor was the only thing that used to catch it,
        # and only when it happened to land far enough away). If exactly one hit is
        # zero, that IS the record -- no need to even reach for the anchor.
        zero = [h for h in hits if read_record(blob, h) == (0, 0)]
        if zero:
            hits = zero
    if len(hits) > 1:
        # A multi-megabyte PokeDNA image carries several MB of compiled (compressed)
        # art; an 8-byte ASCII sequence coincidentally appearing somewhere in it is
        # rare but confirmed to happen in practice (BACKLOG #62: a shiny-sprite blob
        # spelled PDNAGBD1 + 4 zero bytes at a 4-byte-aligned offset in a real build).
        # Disambiguate using proximity to fuse_rom.py's/fuse_sav.py's OWN locator
        # records: every g_pdna_* record is a tiny dependency-free const-volatile
        # struct, and GCC/ld reliably cluster them together in .rodata, while a
        # coincidental match inside a multi-MB art blob lands far away from that
        # cluster.
        anchor = None
        for m in (FUSE_MAGIC, SAV_MAGIC):
            h2 = find_records(blob, m)
            if len(h2) == 1:
                anchor = h2[0]
                break
        if anchor is not None:
            near = [h for h in hits if abs(h - anchor) < 4096]
            if len(near) == 1:
                print(f"  note         : {len(hits)} {MAGIC.decode()} occurrences found; "
                      f"picked the one at 0x{near[0]:X} (within 4 KiB of the other "
                      f"g_pdna_* locator records, the rest are presumably coincidental "
                      f"art-data matches)")
                hits = near
    if len(hits) > 1:
        listing = ", ".join(f"0x{h:X}" for h in hits)
        raise FuseError(
            f"{what}: {len(hits)} {MAGIC.decode()} occurrences at {listing}.\n"
            "  Exactly one must exist BEFORE any payload is appended (a payload's own\n"
            "  bytes may coincidentally contain the magic once fused — that is fine,\n"
            "  this check runs on the clean input)." )
    off = hits[0]
    if off % 4:
        raise FuseError(f"{what}: locator record at 0x{off:X} is not 4-byte aligned.")
    return off


def read_record(blob: bytes, rec_off: int) -> tuple[int, int]:
    return struct.unpack_from("<II", blob, rec_off + 8)


def write_record(buf: bytearray, rec_off: int, off: int, size: int) -> None:
    struct.pack_into("<II", buf, rec_off + 8, off, size)


def classify(path: str) -> int:
    ext = os.path.splitext(path)[1].lower()
    t = EXT_TYPE.get(ext)
    if t is None:
        raise FuseError(f"{path}: unrecognized extension {ext!r} — expected .gb/.gbc/.sav")
    return t


def validate_payload(path: str, t: int, data: bytes) -> None:
    n = len(data)
    if t in (TYPE_ROM_GEN1, TYPE_ROM_GEN2):
        if not (MIN_ROM_SIZE <= n <= MAX_ROM_SIZE):
            raise FuseError(
                f"{path}: {n} bytes — expected {MIN_ROM_SIZE}..{MAX_ROM_SIZE} for a "
                "Game Boy / Game Boy Color ROM.")
    else:
        if not (MIN_SAV_SIZE <= n <= MAX_SAV_SIZE):
            raise FuseError(
                f"{path}: {n} bytes — expected {MIN_SAV_SIZE}..{MAX_SAV_SIZE} for a "
                "Game Boy battery save (32 KiB, up to +{MAX_SAV_TAIL}-byte RTC tail).")


def pack_name(path: str) -> bytes:
    name = os.path.basename(path).encode("ascii", "replace")
    if len(name) > NAME_SIZE - 1:
        raise FuseError(f"{path}: basename longer than {NAME_SIZE - 1} bytes")
    return name + b"\x00" * (NAME_SIZE - len(name))


def build_directory(entries: list[dict]) -> bytes:
    body = bytearray()
    body += MAGIC
    body += struct.pack("<I", len(entries))
    for e in entries:
        body += struct.pack("<I", e["type"])
        body += pack_name(e["path"])
        body += struct.pack("<III", e["offset"], e["size"], e["crc32"])
    dir_size = len(body) + TRAILER_SIZE
    body += struct.pack("<I", dir_size)
    body += MAGIC
    body += struct.pack("<I", 0)
    assert len(body) == dir_size
    return bytes(body)


def parse_directory(blob: bytes, dir_off: int, dir_size: int) -> list[dict]:
    if dir_off + dir_size > len(blob):
        raise FuseError("directory record points past the end of the file")
    block = blob[dir_off:dir_off + dir_size]
    if block[:8] != MAGIC:
        raise FuseError("directory block does not start with PDNAGBD1")
    count = struct.unpack_from("<I", block, 8)[0]
    expect = 12 + ENTRY_SIZE * count + TRAILER_SIZE
    if expect != dir_size:
        raise FuseError(
            f"directory size mismatch: record says {dir_size}, {count} entries "
            f"implies {expect}")
    t_dir_size, t_magic, _ = struct.unpack_from("<I8sI", block, 12 + ENTRY_SIZE * count)
    if t_magic != MAGIC or t_dir_size != dir_size:
        raise FuseError("directory trailer does not match (size or magic mismatch)")
    # The trailer must also be the literal last 16 bytes of the file (see header doc).
    if blob[-TRAILER_SIZE:] != block[-TRAILER_SIZE:]:
        raise FuseError("directory trailer is not the last 16 bytes of the file")
    entries = []
    for i in range(count):
        o = 12 + i * ENTRY_SIZE
        typ, name, off, size, crc = struct.unpack_from("<I32sIII", block, o)
        entries.append({
            "type": typ,
            "name": name.rstrip(b"\x00").decode("ascii", "replace"),
            "offset": off,
            "size": size,
            "crc32": crc,
        })
    return entries


def _append_payload(base: bytearray, entries: list[dict], t: int, name: str, data: bytes) -> None:
    """Appends one payload (real file bytes or a synthetic LOC chunk -- both are just
    bytes to the fused image) at the current 256-aligned end of `base` and records its
    directory entry. Shared by the real ROM/SAV payloads and the BACKLOG #68b LOC
    payloads below so both go through the identical align/offset/crc bookkeeping."""
    pad = (-len(base)) % ALIGN
    base.extend(b"\xFF" * pad)
    off = len(base)
    base.extend(data)
    crc = zlib.crc32(data) & 0xFFFFFFFF
    entries.append({"type": t, "path": name, "offset": off, "size": len(data), "crc32": crc})
    print(f"  + {TYPE_NAMES[t]:9s} {os.path.basename(name):16s} "
          f"{len(data):>9,} B  @ 0x{off:08X}  crc={crc:08X}")


def fuse(pokedna_path: str, payload_paths: list[str], out_path: str, force: bool,
        skip_loc: bool = False) -> int:
    base = bytearray(open(pokedna_path, "rb").read())
    rec_off = locate_record(bytes(base), pokedna_path)
    cur_off, cur_size = read_record(bytes(base), rec_off)
    if (cur_off or cur_size) and not force:
        raise FuseError(
            f"{pokedna_path} already has a GB directory fused (offset 0x{cur_off:X}, "
            f"size {cur_size:,}). Pass --force to replace it, or fuse into a clean build.")
    if cur_off or cur_size:
        if cur_off + cur_size != len(base):
            raise FuseError(
                "--force: the existing GB payload set is not at the end of the file, "
                "so dropping it would corrupt whatever follows. Re-fuse from a clean "
                "build instead.")
        print(f"  --force      : dropping the previous {cur_size}-byte payload set")
        del base[cur_off:]

    entries = []
    print(f"base image   : {pokedna_path} ({len(base):,} bytes)")

    # BACKLOG #68b: built lazily, once, on the first ROM payload -- payloads with no
    # .gb/.gbc never need it, and --no-loc / a payload list with no ROM in it never pay
    # the compile cost at all.
    driver_bin = None
    driver_tmpdir = None

    for p in payload_paths:
        if not os.path.isfile(p):
            raise FuseError(f"{p}: not a file")
        t = classify(p)
        data = open(p, "rb").read()
        validate_payload(p, t, data)
        _append_payload(base, entries, t, p, data)

        if t in (TYPE_ROM_GEN1, TYPE_ROM_GEN2) and not skip_loc:
            if driver_bin is None:
                driver_tmpdir = tempfile.TemporaryDirectory()
                driver_bin = Path(driver_tmpdir.name) / "gbloc_driver"
                build_gbloc_driver(driver_bin)
            for chunk in gbloc_payloads(driver_bin, p):
                kind = chunk[8]
                gen = chunk[9]
                name = f"LOC.{LOC_KIND_NAMES.get(kind, kind)}.g{gen}"
                _append_payload(base, entries, TYPE_LOC, name, chunk)

    if driver_tmpdir is not None:
        driver_tmpdir.cleanup()

    if not entries:
        raise FuseError("no payloads given")

    dir_pad = (-len(base)) % ALIGN
    base.extend(b"\xFF" * dir_pad)
    dir_off = len(base)
    directory = build_directory(entries)
    base.extend(directory)

    total = len(base)
    if total > CART_WINDOW:
        raise FuseError(
            f"fused image would be {total:,} bytes ({total / 2**20:.2f} MiB) — over the "
            f"{CART_WINDOW // 2**20} MiB GBA cartridge window. Drop a payload.")

    write_record(base, rec_off, dir_off, len(directory))

    tmp = out_path + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(base)
    os.replace(tmp, out_path)

    # Verify by re-reading — never trust the in-memory buffer.
    back = open(out_path, "rb").read()
    problems = []
    if len(back) != total:
        problems.append(f"size is {len(back):,}, expected {total:,}")
    # Read the record at the KNOWN offset directly -- do not re-locate by magic here:
    # once payloads are appended the directory's own magic (and possibly a payload's
    # incidental bytes) can make MAGIC occur more than once in the whole file, which
    # locate_record() correctly refuses. rec_off was already established as the one
    # true record location before any payload was appended.
    got_off, got_size = read_record(back, rec_off)
    if got_off != dir_off or got_size != len(directory):
        problems.append(
            f"record reads back as (0x{got_off:X}, {got_size}), expected "
            f"(0x{dir_off:X}, {len(directory)})")
    try:
        back_entries = parse_directory(back, dir_off, len(directory))
    except FuseError as e:
        problems.append(f"directory did not round-trip: {e}")
        back_entries = []
    for want, got in zip(entries, back_entries):
        if (got["type"], got["offset"], got["size"], got["crc32"]) != \
           (want["type"], want["offset"], want["size"], want["crc32"]):
            problems.append(f"entry {want['path']} did not round-trip")
        payload = back[got["offset"]:got["offset"] + got["size"]]
        if (zlib.crc32(payload) & 0xFFFFFFFF) != want["crc32"]:
            problems.append(f"entry {want['path']} payload CRC mismatch on re-read")
    if problems:
        raise FuseError("VERIFY FAILED:\n  - " + "\n  - ".join(problems))

    print(f"\ndirectory    : 0x{dir_off:08X} ({len(directory)} bytes, {len(entries)} entries)")
    print(f"record at    : 0x{rec_off:X} (cart address 0x{CART_BASE + rec_off:08X})")
    print(f"output       : {out_path} ({total:,} bytes, {total / 2**20:.2f} MiB)")
    print("VERIFY: OK — directory round-trips, every payload CRC matches")
    if any(t == TYPE_SAV for t in (e["type"] for e in entries)):
        print("REMINDER: this image embeds personal save data. Do not publish it.")
    if any(t in (TYPE_ROM_GEN1, TYPE_ROM_GEN2) for t in (e["type"] for e in entries)):
        print("REMINDER: this image embeds commercial Game Boy ROM(s). Do not publish it.")
    return 0


def _record_is_valid_directory(blob: bytes, rec_off: int) -> bool:
    """True iff the (offset,size) pair stored AT rec_off itself parses as a
    structurally valid PDNAGBD1 directory (magic/count/trailer all self-consistent,
    parse_directory() does the real checking) -- the one positive signal `hits[0]`
    alone never checked at all (#62 review D7)."""
    try:
        off, size = read_record(blob, rec_off)
        if not size:
            return False
        parse_directory(blob, off, size)
        return True
    except (FuseError, struct.error):
        return False


def locate_record_permissive(blob: bytes, what: str) -> int:
    """Like locate_record(), but for a FUSED file: once a directory is appended, MAGIC
    legitimately occurs at least 3 times (the record itself, the directory block's own
    leading magic, and its trailer's restated magic) -- possibly more if a payload's
    bytes happen to contain it (an art-blob collision, same as locate_record()'s own
    BACKLOG #62 case, is just as possible here).

    #62 review D7: the FIRST occurrence is NOT reliably the true record -- an art
    collision can land anywhere in the file, including before the real record. Prefer
    whichever occurrence's own (offset,size) pair parses as a structurally valid
    directory (_record_is_valid_directory): that is a positive, checkable signal a bare
    first-hit guess never was. Falls back to proximity-to-anchor disambiguation (same
    heuristic locate_record() uses), then the first occurrence, only when no candidate
    validates -- e.g. --check on a not-yet-fused image, where every candidate's size is
    legitimately 0 and none can parse as a directory."""
    hits = find_records(blob, MAGIC)
    if not hits:
        raise FuseError(f"{what}: no {MAGIC.decode()} locator record found.")
    if len(hits) > 1:
        valid = [h for h in hits if _record_is_valid_directory(blob, h)]
        if len(valid) == 1:
            hits = valid
        elif not valid:
            anchor = None
            for m in (FUSE_MAGIC, SAV_MAGIC):
                h2 = find_records(blob, m)
                if len(h2) == 1:
                    anchor = h2[0]
                    break
            if anchor is not None:
                near = [h for h in hits if abs(h - anchor) < 4096]
                if len(near) == 1:
                    hits = near
        else:
            hits = valid   # more than one validates -- keep the first, same as before
    off = hits[0]
    if off % 4:
        raise FuseError(f"{what}: locator record at 0x{off:X} is not 4-byte aligned.")
    return off


def check_only(path: str) -> int:
    blob = open(path, "rb").read()
    rec_off = locate_record_permissive(blob, path)
    off, size = read_record(blob, rec_off)
    print(f"file         : {path} ({len(blob):,} bytes)")
    print(f"record at    : 0x{rec_off:X}")
    if not size:
        print("fused GB dir : none (offset/size are 0)")
        return 0
    print(f"dir offset   : 0x{off:08X}  dir size: {size}")
    try:
        entries = parse_directory(blob, off, size)
    except FuseError as e:
        print(f"PROBLEMS: {e}")
        return 1
    print(f"entries      : {len(entries)}")
    bad = 0
    for e in entries:
        payload = blob[e["offset"]:e["offset"] + e["size"]]
        crc = zlib.crc32(payload) & 0xFFFFFFFF
        ok = crc == e["crc32"]
        extra = ""
        # BACKLOG #68b: a LOC entry's own [header+record] payload carries a second,
        # independent claim (kind/gen/id_hash/rom_size) about the ROM it accompanies --
        # verify that claim against the ACTUAL fused ROM bytes, not just the outer CRC
        # (which only proves the LOC bytes themselves are intact, not that they still
        # describe the right ROM).
        if ok and e["type"] == TYPE_LOC:
            if len(payload) < LOC_HDR_SIZE or payload[:8] != LOC_HDR_MAGIC:
                ok = False
                extra = " (malformed LOC header)"
            else:
                kind, gen = payload[8], payload[9]
                rec_size = struct.unpack_from("<H", payload, 10)[0]
                claimed_hash, claimed_size = struct.unpack_from("<II", payload, 12)
                if LOC_HDR_SIZE + rec_size != len(payload):
                    ok = False
                    extra = " (rec_size does not match payload length)"
                else:
                    rom_type = TYPE_ROM_GEN1 if gen == 1 else TYPE_ROM_GEN2
                    rom_entries = [x for x in entries if x["type"] == rom_type]
                    match = next((x for x in rom_entries if x["size"] == claimed_size), None)
                    if match is None:
                        ok = False
                        extra = f" (no gen-{gen} ROM entry of size {claimed_size})"
                    else:
                        rom_bytes = blob[match["offset"]:match["offset"] + match["size"]]
                        real_hash = gb_id_hash(rom_bytes)
                        if real_hash != claimed_hash:
                            ok = False
                            extra = f" (id_hash mismatch vs {match['name']})"
                        else:
                            extra = (f" ({LOC_KIND_NAMES.get(kind, kind)} gen={gen} "
                                     f"-> {match['name']})")
        bad += not ok
        print(f"  {TYPE_NAMES.get(e['type'], '?'):9s} {e['name']:16s} "
              f"{e['size']:>9,} B  @ 0x{e['offset']:08X}  crc={e['crc32']:08X}  "
              f"{'OK' if ok else 'CRC MISMATCH'}{extra}")
    if bad:
        print(f"PROBLEMS: {bad} payload(s) failed CRC")
        return 1
    print("looks consistent")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("pokedna", nargs="?")
    ap.add_argument("payloads", nargs="*")
    ap.add_argument("-o", "--output")
    ap.add_argument("--check", metavar="FUSED.GBA")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--no-loc", action="store_true",
                    help="BACKLOG #68b: skip computing/fusing rom_gb*_open_loc() records "
                         "for the fused ROM(s) -- used to build the cold-start comparison "
                         "image for tools/dgb_shots.py")
    a = ap.parse_args(argv)

    try:
        if a.check:
            return check_only(a.check)
        if not (a.pokedna and a.payloads and a.output):
            ap.error("need <pokedna.gba> PAY [PAY ...] -o <out.gba>, or --check FILE")
        if not os.path.isfile(a.pokedna):
            sys.exit(f"{a.pokedna}: not a file")
        return fuse(a.pokedna, a.payloads, a.output, a.force, skip_loc=a.no_loc)
    except FuseError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    sys.exit(main())
