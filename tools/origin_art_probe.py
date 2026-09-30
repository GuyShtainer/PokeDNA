#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""origin_art_probe.py -- BACKLOG #299: does a Gen-3 cell's art follow its ORIGIN GAME?

Builds four fused images from ONE delta-artless build and the corpus (REAL records, nothing
forged -- tools/plant_gen3_record.c copies a genuine FireRed-caught / Emerald-caught Magnemite
verbatim into slot 1/2 of a COPY of the other game's save and re-checksums):

    Emerald session (Emerald.sav + FR-origin Magnemite)  x  Emerald ROM | FireRed ROM
    FireRed session (FireRed.sav + EM-origin Magnemite)  x  FireRed ROM | Emerald ROM

For each session it opens the summary (front, then SELECT = back) of the Emerald-origin and
the FireRed-origin Magnemite and compares their portraits (min pixel diff over 24 animation
phases).  Expected, and asserted: origin vs origin = 0 px in EVERY image (the art never reads
the origin game), ROM swap != 0 px (the art is the REGISTERED ROM's).  Exit 1 if not.

    /usr/local/bin/python3 tools/origin_art_probe.py --image pokedna-delta-artless.gba --roms /path/to/roms
"""
import argparse
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "source"
sys.path.insert(0, str(HERE))
import gb_shots  # noqa: E402

BOX = (4, 8, 92, 80)   # the summary portrait
OUT = Path("/tmp/origin_art_probe")
MAGNEMITE = "81"
C_SRCS = ["gen3_mon", "gen3_save", "gen3_box", "gen3_clip", "gen3_edit", "gen3_daycare", "data_tables"]


def run(*cmd):
    subprocess.run([str(c) for c in cmd], check=True, stdout=subprocess.DEVNULL)


def build_inputs(image: Path, roms: Path) -> dict:
    OUT.mkdir(exist_ok=True)
    helper = OUT / "plant_gen3_record"
    run("cc", "-std=c11", "-O1", f"-I{SRC}", HERE / "plant_gen3_record.c", *[SRC / f"{s}.c" for s in C_SRCS], "-o", helper)
    run(helper, "dump", roms / "FireRed.sav", MAGNEMITE, OUT / "fr81.bin")
    run(helper, "dump", roms / "Emerald.sav", MAGNEMITE, OUT / "em81.bin")
    run(helper, "put", roms / "Emerald.sav", OUT / "fr81.bin", 0, 1, OUT / "em_with_fr.sav")      # slot0 Em-origin, slot1 FR-origin
    run(helper, "put", roms / "FireRed.sav", OUT / "em81.bin", 0, 1, OUT / "fr_tmp.sav")
    run(helper, "put", OUT / "fr_tmp.sav", OUT / "fr81.bin", 0, 2, OUT / "fr_with_em.sav")         # slot1 Em-origin, slot2 FR-origin
    imgs = {}
    for rom in ("Emerald", "FireRed"):
        run("python3", HERE / "fuse_rom.py", image, roms / f"{rom}.gba", "-o", OUT / f"rom_{rom}.gba")
        for sess, sav in (("E", "em_with_fr"), ("F", "fr_with_em")):
            out = OUT / f"{sess}_{rom}.gba"
            run("python3", HERE / "fuse_sav.py", OUT / f"rom_{rom}.gba", OUT / f"{sav}.sav", "-o", out)
            imgs[(sess, rom)] = out
    return imgs


def frames(s, n=24, step=5):
    out = []
    for _ in range(n):
        out.append(np.asarray(s.screen.to_pil().convert("RGB").crop(BOX)).astype(int))
        s.run(step)
    return out


def grab(core_mod, image_mod, img, rights, back):
    s = gb_shots.Session(core_mod, image_mod, Path(img), OUT, "p_")
    s.run(250)
    for _ in range(rights):
        s.tap("RIGHT", settle=30)
    s.tap("A", settle=40)
    s.tap("A", settle=40)
    s.run(300)
    if back:
        s.tap("SEL", settle=40)
        s.run(200)
    return frames(s)


def mind(a, b):
    return min(int((np.abs(x - y).max(axis=2) > 0).sum()) for x in a for y in b)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", type=Path, required=True)
    ap.add_argument("--roms", type=Path, required=True)
    args = ap.parse_args()
    imgs = build_inputs(args.image, args.roms)
    core_mod, image_mod = gb_shots.load_mgba()
    bad = 0
    pics = {}
    # (session, [rights to the Em-origin mon, rights to the FR-origin mon])
    for (sess, rom), img in sorted(imgs.items()):
        em_r, fr_r = (0, 1) if sess == "E" else (1, 2)
        for back in (0, 1):
            em = grab(core_mod, image_mod, img, em_r, back)
            fr = grab(core_mod, image_mod, img, fr_r, back)
            d = mind(em, fr)
            pics[(sess, rom, back)] = (em, fr)
            ok = d == 0
            bad += not ok
            print(f"{sess}-session {rom:8s}-ROM {'back ' if back else 'front'}: Em-origin vs FR-origin = {d:4d} px  {'ok' if ok else 'FAIL (art reads origin)'}")
    for sess in ("E", "F"):
        for back in (0, 1):
            a = pics[(sess, "Emerald", back)][0]
            b = pics[(sess, "FireRed", back)][0]
            d = mind(a, b)
            ok = d != 0
            bad += not ok
            print(f"{sess}-session {'back ' if back else 'front'}: Emerald-ROM vs FireRed-ROM art = {d:4d} px  {'ok' if ok else 'FAIL (ROM swap changed nothing -- probe is blind)'}")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
