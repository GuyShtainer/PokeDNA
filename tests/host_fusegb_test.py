#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""host_fusegb_test.py — round-trip test for tools/fuse_gb.py (BACKLOG #62).

Builds a tiny fake "PokeDNA" image (just the g_pdna_gbd locator record's magic bytes
padded to look like a real .gba), fuses a handful of synthetic GB ROM/save payloads
into it, and verifies: the directory round-trips byte-for-byte, every entry's type/
name/offset/size is exactly what was fused, and every payload's CRC-32 matches. Also
exercises --check and the "already fused, needs --force" refusal.

Standalone (not a pure-C test — fuse_gb.py is python), not wired into
run_host_tests.py, which only builds/runs the C host suite. Run directly:

    python3 tests/host_fusegb_test.py
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FUSE_GB = os.path.join(ROOT, "tools", "fuse_gb.py")

MAGIC = b"PDNAGBD1"
ENTRY_SIZE = 48


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def make_fake_pokedna(path, size=4096):
    # A locator record shaped exactly like g_pdna_gbd: magic(8) + offset(4) + size(4),
    # both zero (unfused), embedded once inside otherwise-arbitrary padding.
    buf = bytearray(b"\xAA" * size)
    rec_off = 512
    buf[rec_off:rec_off + 8] = MAGIC
    struct.pack_into("<II", buf, rec_off + 8, 0, 0)
    with open(path, "wb") as fh:
        fh.write(buf)
    return rec_off


def run(*args):
    r = subprocess.run([sys.executable, FUSE_GB, *args], capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def read_record(blob, rec_off):
    return struct.unpack_from("<II", blob, rec_off + 8)


def parse_directory(blob, off, size):
    block = blob[off:off + size]
    assert block[:8] == MAGIC
    count = struct.unpack_from("<I", block, 8)[0]
    entries = []
    for i in range(count):
        o = 12 + i * ENTRY_SIZE
        typ, name, eoff, esize, crc = struct.unpack_from("<I32sIII", block, o)
        entries.append((typ, name.rstrip(b"\x00").decode(), eoff, esize, crc))
    trailer_off = 12 + ENTRY_SIZE * count
    t_size, t_magic, _ = struct.unpack_from("<I8sI", block, trailer_off)
    assert t_size == size, f"trailer size {t_size} != record size {size}"
    assert t_magic == MAGIC, "trailer magic mismatch"
    assert blob[-16:] == block[-16:], "trailer is not the last 16 bytes of the file"
    return entries


def main():
    with tempfile.TemporaryDirectory() as td:
        pokedna = os.path.join(td, "fake.gba")
        rec_off = make_fake_pokedna(pokedna)

        rom1 = os.path.join(td, "Red.gb")
        with open(rom1, "wb") as fh:
            fh.write(bytes((i * 7) & 0xFF for i in range(0x8000)))  # 32 KiB, min ROM size
        sav1 = os.path.join(td, "Red.sav")
        with open(sav1, "wb") as fh:
            fh.write(bytes((i * 13) & 0xFF for i in range(0x8000)))
        rom2 = os.path.join(td, "Gold.gbc")
        with open(rom2, "wb") as fh:
            fh.write(bytes((i * 3 + 1) & 0xFF for i in range(0x10000)))  # 64 KiB
        sav2 = os.path.join(td, "Gold.sav")
        with open(sav2, "wb") as fh:
            fh.write(bytes((i * 5 + 2) & 0xFF for i in range(0x8010)))  # 32 KiB + 16 B tail

        out = os.path.join(td, "out.gba")
        rc, so, se = run(pokedna, rom1, sav1, rom2, sav2, "-o", out)
        if rc != 0:
            fail(f"fuse failed: rc={rc}\nstdout={so}\nstderr={se}")
        if "VERIFY: OK" not in so:
            fail(f"no VERIFY: OK in output:\n{so}")

        blob = open(out, "rb").read()
        off, size = read_record(blob, rec_off)
        if not off or not size:
            fail("record still reads as unfused after fuse")
        entries = parse_directory(blob, off, size)
        if len(entries) != 4:
            fail(f"expected 4 entries, got {len(entries)}")

        want = [
            (1, "Red.gb", open(rom1, "rb").read()),
            (3, "Red.sav", open(sav1, "rb").read()),
            (2, "Gold.gbc", open(rom2, "rb").read()),
            (3, "Gold.sav", open(sav2, "rb").read()),
        ]
        for (typ, name, eoff, esize, crc), (wtyp, wname, wdata) in zip(entries, want):
            if typ != wtyp or name != wname or esize != len(wdata):
                fail(f"entry mismatch: got ({typ},{name},{esize}) want ({wtyp},{wname},{len(wdata)})")
            payload = blob[eoff:eoff + esize]
            if payload != wdata:
                fail(f"{name}: payload bytes do not match input")
            if (zlib.crc32(payload) & 0xFFFFFFFF) != crc:
                fail(f"{name}: CRC-32 mismatch")
        print(f"  round-trip   : 4 entries, all payloads byte-exact + CRC-verified")

        # --check on the fused file should succeed and report all 4 as OK.
        rc, so, se = run("--check", out)
        if rc != 0:
            fail(f"--check failed on a good file: rc={rc}\n{so}\n{se}")
        if so.count("OK") < 4:
            fail(f"--check did not report 4 OK entries:\n{so}")
        print("  --check      : reports all 4 payloads OK")

        # Re-fusing without --force must refuse.
        rc, so, se = run(pokedna if False else out, rom1, "-o", out + "2")
        # out already has a directory; fusing again onto `out` should refuse.
        rc, so, se = run(out, rom1, "-o", os.path.join(td, "again.gba"))
        if rc == 0:
            fail("re-fusing an already-fused image without --force should have refused")
        print("  --force gate : refuses to re-fuse without --force")

        # Corrupt one payload byte and confirm --check catches the CRC mismatch.
        bad = bytearray(blob)
        e0_off = entries[0][2]
        bad[e0_off] ^= 0xFF
        bad_path = os.path.join(td, "corrupt.gba")
        with open(bad_path, "wb") as fh:
            fh.write(bad)
        rc, so, se = run("--check", bad_path)
        if rc == 0 or "CRC MISMATCH" not in so:
            fail(f"--check did not catch a corrupted payload:\n{so}")
        print("  CRC detection: --check catches a flipped payload byte")

        # #62 review D9: a single 32 MiB payload is rejected by validate_payload's own
        # per-file MAX_ROM_SIZE (8 MiB) before the CART_WINDOW total check ever runs --
        # this covers the per-file cap, not the window cap.
        huge = os.path.join(td, "huge.gb")
        with open(huge, "wb") as fh:
            fh.write(b"\x00" * (32 * 1024 * 1024))
        rc, so, se = run(pokedna, huge, "-o", os.path.join(td, "toobig.gba"))
        if rc == 0:
            fail("a single payload over MAX_ROM_SIZE should have been refused")
        print("  per-file cap : refuses a single payload over MAX_ROM_SIZE (8 MiB)")

        # #62 review D9: the REAL window-cap case -- several payloads each UNDER
        # MAX_ROM_SIZE (8 MiB), summing past the 32 MiB CART_WINDOW. The per-file check
        # above never fires; only the running-total check (fuse_gb.py's own `if total >
        # CART_WINDOW`) can catch this.
        under_cap = 7 * 1024 * 1024   # 7 MiB each, < MAX_ROM_SIZE
        big_roms = []
        for i in range(5):            # 5 * 7 MiB = 35 MiB > 32 MiB CART_WINDOW
            p = os.path.join(td, f"big{i}.gb")
            with open(p, "wb") as fh:
                fh.write(b"\x00" * under_cap)
            big_roms.append(p)
        pokedna2 = os.path.join(td, "fake2.gba")
        make_fake_pokedna(pokedna2)
        rc, so, se = run(pokedna2, *big_roms, "-o", os.path.join(td, "toobig2.gba"))
        if rc == 0:
            fail("payloads summing past the 32 MiB cartridge window should have been refused")
        if "cartridge window" not in (so + se):
            fail(f"window-cap refusal did not name the cartridge window:\n{so}\n{se}")
        print("  window cap   : refuses payloads that individually pass but sum past 32 MiB")

        # #62 review D7/D9: the documented collision -- an art-blob byte sequence that
        # coincidentally spells the locator magic, with a NONZERO (offset,size) trailing
        # it, alongside the real (never-fused) (0,0) record. Deliberately WITHOUT a
        # fuse_rom.py/fuse_sav.py anchor record nearby, so the OLD code (proximity-only)
        # would have failed here with "2 occurrences" -- D7's zero-filter must
        # disambiguate on its own, with no anchor to fall back on.
        collide = bytearray(b"\xBB" * 8192)
        real_rec = 512
        collide[real_rec:real_rec + 8] = MAGIC
        struct.pack_into("<II", collide, real_rec + 8, 0, 0)          # the real, unfused record
        decoy_rec = 4096
        collide[decoy_rec:decoy_rec + 8] = MAGIC
        struct.pack_into("<II", collide, decoy_rec + 8, 0, 43459)     # BACKLOG #62's own collision shape
        pokedna3 = os.path.join(td, "collide.gba")
        with open(pokedna3, "wb") as fh:
            fh.write(bytes(collide))
        out3 = os.path.join(td, "collide_out.gba")
        rc, so, se = run(pokedna3, rom1, sav1, "-o", out3)
        if rc != 0:
            fail(f"D7 zero-filter should have disambiguated the collision on its own:\n{so}\n{se}")
        blob3 = open(out3, "rb").read()
        off3, size3 = read_record(blob3, real_rec)
        if not off3 or not size3:
            fail("collision case: the REAL (0,0) record was not the one patched")
        # The decoy record must be untouched -- it was never a real locator.
        decoy_off, decoy_size = struct.unpack_from("<II", blob3, decoy_rec + 8)
        if (decoy_off, decoy_size) != (0, 43459):
            fail("collision case: the decoy record was modified -- wrong record was patched")
        print("  collision    : D7's zero-filter picks the real (0,0) record with no anchor nearby")

    print("host_fusegb_test: ALL OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
