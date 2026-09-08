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

        # Total-size cap: a payload that would push the image past 32 MiB must be refused.
        huge = os.path.join(td, "huge.gb")
        with open(huge, "wb") as fh:
            fh.write(b"\x00" * (32 * 1024 * 1024))
        rc, so, se = run(pokedna, huge, "-o", os.path.join(td, "toobig.gba"))
        if rc == 0:
            fail("a payload pushing the image past 32 MiB should have been refused")
        print("  32 MiB cap   : refuses an oversized fuse")

    print("host_fusegb_test: ALL OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
