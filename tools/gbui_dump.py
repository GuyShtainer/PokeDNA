#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""gbui_dump.py — print docs/GB-GAME-SCREENS-DESIGN.md Appendix A's table for a
given Game Boy ROM by running the SHIPPED source/rom_gbui.c, not a Python
re-implementation. Compiles source/rom_gbui.c + source/gb_sprite_codec.c +
tools/gbui_dump_driver.c on the host (same posture as
tests/host_romgbui_test.c: the code under test is byte-for-byte what the GBA
build locates) and parses the driver's stdout.

Usage:
    python3 tools/gbui_dump.py roms/gb/Red.gb
    python3 tools/gbui_dump.py roms/gb/Crystal.gbc --out /tmp/gbui_png

`--out DIR` additionally asks the driver to write one .ppm per located
graphic into DIR (created if missing). DIR must be OUTSIDE the repo -- this
script refuses a path under the project root, because these images are
decoded Game Freak pixel art (the whole point of PokeDNA's design is that
nothing like that is ever committed).

CLEAN-ROOM: this script does not locate anything itself -- it only invokes
the already-reviewed C locator and prints/renders what it found.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ITEM_NOTES = {
    "font": "128 tiles, 1bpp; verify: 0 solid, <=40 blank",
    "textbox": "32 tiles, 2bpp; verify: 32 distinct",
    "cardframe": "640 B (9+22+1+8 tiles), 2bpp; verify: 9 distinct/22 blank/8 non-degenerate, unique bank",
    "badges": "64t (Gen1) / 44t (Gen2), 2bpp",
    "leaders": "86 tiles loaded, 2bpp (Gen 2 only)",
    "playerpic": "Gen-1 compressed pic; verify: first byte 0x77, decodes 56x56",
    "playerpic_bank": "the bank playerpic's blob lives in",
    "frames": "9 x 6 tiles, 1bpp (Gen 2 only); verify: >=46 distinct of 54",
    "fontextra": "32 tiles, 2bpp; derived Font-512",
    "cardpic_m": "35 tiles, 2bpp, col-major (Chris)",
    "cardpic_f": "35 tiles, 2bpp, col-major (Kris, Crystal only)",
    "cardgfx": "6 tiles, 2bpp (TrainerCardGFX)",
    "pack_m": "60 tiles, 2bpp (PackGFX)",
    "pack_f": "60 tiles, 2bpp (PackFGFX, Crystal only)",
    "g1_keyitems": "BACKLOG #99: IsKeyItem_'s KeyItemFlags, 15 B (ids 1..120), Gen 1 only, OPTIONAL",
}
ORDER = ["gen", "banks", "font", "textbox", "cardframe", "badges", "leaders",
         "playerpic", "playerpic_bank", "frames", "fontextra",
         "cardpic_m", "cardpic_f", "cardgfx", "pack_m", "pack_f", "g1_keyitems"]


def build_driver(bin_path: Path) -> None:
    cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-I", str(ROOT / "source"),
           str(ROOT / "tools" / "gbui_dump_driver.c"),
           str(ROOT / "source" / "rom_gbui.c"),
           str(ROOT / "source" / "gb_sprite_codec.c"),
           "-o", str(bin_path)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(r.stdout); print(r.stderr, file=sys.stderr)
        sys.exit(1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--out", help="write .ppm images here (must be OUTSIDE the repo)")
    args = ap.parse_args()

    rom = Path(args.rom).resolve()
    if not rom.exists():
        print(f"not present: {rom}")
        return 0

    out_dir = None
    if args.out:
        out_dir = Path(args.out).resolve()
        try:
            out_dir.relative_to(ROOT)
            print("refusing --out inside the repo (decoded pixel art is never committed)")
            return 2
        except ValueError:
            pass
        out_dir.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as td:
        binp = Path(td) / "gbui_dump_driver"
        build_driver(binp)
        cmd = [str(binp), str(rom)]
        if out_dir:
            cmd.append(str(out_dir))
        r = subprocess.run(cmd, capture_output=True, text=True)
        fields: dict[str, str] = {}
        result = "?"
        for line in r.stdout.splitlines():
            if "\t" not in line:
                continue
            k, v = line.split("\t", 1)
            if k == "RESULT":
                result = v
            else:
                fields[k] = v

    print(f"=== {rom.name} ===  RESULT={result}")
    for k in ORDER:
        if k not in fields:
            continue
        v = fields[k]
        located = v not in ("0x0",)
        note = ITEM_NOTES.get(k, "")
        print(f"  {k:<16} {v:<10} {'located' if located else '-':<9} {note}")
    if "g1_keyitems_bitmap" in fields:
        bm = fields["g1_keyitems_bitmap"]
        ids = [str(i + 1) for i in range(len(bm)) if bm[i] == "1"]
        print(f"  g1_keyitems_bitmap ({len(bm)} ids, {len(ids)} set):")
        print(f"    set ids: {' '.join(ids)}")
    if out_dir:
        n = len(list(out_dir.glob("*.ppm")))
        print(f"  wrote {n} .ppm file(s) to {out_dir}")
    return 0 if result == "OK" else 1


if __name__ == "__main__":
    sys.exit(main())
