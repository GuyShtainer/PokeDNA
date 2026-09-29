#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""y2_boxgrid_shots.py -- lane y2-boxgrid's shot + counter harness (BACKLOG #262/#263/#264/#268).

Drives one or more fused delta images (tools/fuse_gb.py <delta-artless.gba> ROM SAV) through the SAME
key script on the Game Boy box grid and either

  sheet   -- takes a frame at every `shot` token, prints a PER-TAP TRACE (tap index, key, crc32 of
             the frame after the settle, changed-vs-previous), and composes a contact sheet:
             one row per shot, one column per image, each frame 2x, labelled.
  count   -- runs the script, taps B once to leave the box (which flushes the rollup) and prints
             the `perf box.era xN ... sd Nr/Ns ... icons H/M mru` line the run produced -- the
             emulator's stand-in for reading /PokeDNA/log.txt off the cart. `sd` in the delta
             build is a MODEL (fused_gb.c): see the 263 commit message for what it excludes.
  vram    -- runs the script on TWO images and compares the raw Mode-3 VRAM bitmap (240x160
             halfwords, read straight out of mGBA, no sprites) after every tap: "is the BG layer
             pixel-identical, before vs after?" -- the proof used for #268.

Usage (tokens: a key name from gb_shots.KEY, or `shot` / `shot:<name>`):
  tools/y2_boxgrid_shots.py sheet --which yellow --out DIR before=A.gba after=B.gba -- L L L shot UP shot
  tools/y2_boxgrid_shots.py count --which yellow A.gba -- L L L UP DOWN
  tools/y2_boxgrid_shots.py vram  --which yellow A.gba B.gba -- L L L UP DOWN

Needs the mGBA python vendor tree at ../rec2mp4/vendor (gb_shots.load_mgba), like every dgb chain.
The images embed commercial ROMs: never commit or publish them.
"""
from __future__ import annotations

import argparse
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots    # noqa: E402
import dgb_shots   # noqa: E402

SETTLE = 200       # frames after a tap: long enough for a whole box repaint to finish


class _Log:
    """Capture the app's own log lines (perf rollups) from mGBA's debug channel."""
    def __init__(self, log_mod):
        self.lines: list[str] = []
        outer = self

        class Cap(log_mod.Logger):
            def log(self, category, level, message):      # noqa: A002
                try:
                    m = log_mod.ffi.string(message).decode("utf-8", "replace")
                except TypeError:
                    m = str(message)
                if not m.startswith(("SWI", "Write to GPIO", "Starting DMA")):
                    outer.lines.append(m)
        log_mod.install_default(Cap())


def _boot(core_mod, image_mod, img: Path, which: str, out: Path, tag: str) -> gb_shots.Session:
    s = gb_shots.Session(core_mod, image_mod, img, out, tag)
    dgb_shots.boot_to_gb_session(s, img, which=which)
    return s


def _frame_crc(png: Path) -> int:
    from PIL import Image
    return zlib.crc32(Image.open(png).convert("RGB").tobytes())


def _vram(core) -> list[int]:
    m = core.memory
    return [m.u16[0x06000000 + 2 * i] for i in range(240 * 160)]


def cmd_sheet(a, core_mod, image_mod) -> int:
    from PIL import Image, ImageDraw
    a.out.mkdir(parents=True, exist_ok=True)
    cols = [tuple(x.split("=", 1)) for x in a.images]
    frames: dict[str, list[tuple[str, Path]]] = {}
    for label, img in cols:
        s = _boot(core_mod, image_mod, Path(img), a.which, a.out, f"{label}_")
        print(f"== {label}: {img}")
        last = None
        n = 0
        for i, tok in enumerate(a.keys):
            if tok.startswith("shot"):
                name = tok.split(":", 1)[1] if ":" in tok else f"s{n}"
                p = s.shot(f"{n:02d}_{name}", f"{label} {name}", allow_same=True)
                frames.setdefault(label, []).append((name, p))
                n += 1
                continue
            s.tap(tok, settle=SETTLE)
            p = s.shot(f"tap{i:02d}_{tok}", f"{label} after tap {i} {tok}", allow_same=True)
            crc = _frame_crc(p)
            print(f"  tap {i:2d} {tok:5s} settle={SETTLE} crc={crc:08x} "
                  f"{'CHANGED' if crc != last else 'same'}")
            last = crc
    rows = max(len(v) for v in frames.values())
    W, H, PAD, CAP = 240 * 2, 160 * 2, 4, 14
    sheet = Image.new("RGB", (len(cols) * (W + PAD) + PAD, rows * (H + CAP + PAD) + PAD), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for ci, (label, _) in enumerate(cols):
        for ri, (name, p) in enumerate(frames[label]):
            im = Image.open(p).convert("RGB").resize((W, H), Image.NEAREST)
            x, y = PAD + ci * (W + PAD), PAD + ri * (H + CAP + PAD)
            sheet.paste(im, (x, y + CAP))
            d.text((x + 2, y + 1), f"{label}: {name}", fill=(255, 255, 255))
    out = a.out / a.sheet_name
    sheet.save(out)
    print(f"sheet: {out}  ({len(cols)} col x {rows} row)")
    return 0


def cmd_count(a, core_mod, image_mod) -> int:
    import mgba.log as log_mod
    lg = _Log(log_mod)
    a.out.mkdir(parents=True, exist_ok=True)
    s = _boot(core_mod, image_mod, Path(a.images[0]), a.which, a.out, "cnt_")
    mark = len(lg.lines)
    for tok in a.keys:
        s.tap(tok, settle=SETTLE)
    s.tap("B", settle=SETTLE)           # leaving the box flushes the rollups
    for l in lg.lines[mark:]:
        if l.startswith("perf box.") or l.startswith("perf bob.box"):
            print(l)
    return 0


def cmd_vram(a, core_mod, image_mod) -> int:
    a.out.mkdir(parents=True, exist_ok=True)
    runs = []
    for k, img in enumerate(a.images[:2]):
        s = _boot(core_mod, image_mod, Path(img), a.which, a.out, f"v{k}_")
        seq = []
        for tok in a.keys:
            s.tap(tok, settle=SETTLE)
            seq.append(_vram(s.core))
        runs.append(seq)
    total = 0
    for i, (x, y) in enumerate(zip(*runs)):
        diff = [j for j in range(240 * 160) if x[j] != y[j]]
        total += len(diff)
        print(f"  tap {i:2d} {a.keys[i]:5s} bitmap px differing: {len(diff)}"
              + (f"  first {[(j % 240, j // 240) for j in diff[:4]]}" if diff else ""))
    print(f"TOTAL differing pixels: {total}")
    return 0 if total == 0 else 1


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("sheet", "count", "vram"))
    ap.add_argument("--which", required=True, help="save stem in the fused image (yellow/red/crystal/gold)")
    ap.add_argument("--out", type=Path, default=Path("/tmp/y2_boxgrid_shots"))
    ap.add_argument("--sheet-name", default="sheet.png")
    ap.add_argument("images", nargs="+", help="fused image paths; for `sheet`: label=path")
    ap.add_argument("--", dest="_", nargs="*")     # placeholder, real split below
    raw = list(sys.argv[1:] if argv is None else argv)
    if "--" in raw:
        i = raw.index("--")
        keys, raw = raw[i + 1:], raw[:i]
    else:
        keys = []
    a = ap.parse_args(raw)
    a.keys = keys
    core_mod, image_mod = gb_shots.load_mgba()
    return {"sheet": cmd_sheet, "count": cmd_count, "vram": cmd_vram}[a.mode](a, core_mod, image_mod)


if __name__ == "__main__":
    sys.exit(main())
