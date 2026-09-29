#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""y3_create_shots.py -- lane y3-create's CREATE-flow shot chain (BACKLOG #265/#266/#272).

Boots a fused delta image (tools/fuse_gb.py) to the Game Boy box grid with the SAME boot prefix every
dgb chain uses, then runs a token script and writes a per-tap trace (crc32 of the settled frame, changed
vs previous), one PNG per `shot`, and a contact sheet.

Tokens:
  KEY            one tap, settle 200          (KEY = a gb_shots.KEY name: A B SEL START UP DOWN L R ...)
  KEY*N          N taps of KEY, settle 30 each (list scrolling)
  KEY:S / KEY*N:S   same with settle S frames
  wait:N         run N frames, no key
  shot:name      take a frame; `shot:name=text1|text2` also asserts each text is READ off the frame
                 (gb_claims) -- the caption is a mechanical claim, not prose (rule 17 / BACKLOG #184)

Usage: tools/y3_create_shots.py --which yellow --out DIR IMAGE.gba -- DOWN A A DOWN*143 shot:pick=ARTICUNO
Needs the mGBA python vendor tree at ../rec2mp4/vendor. The images embed commercial ROMs/saves: never
commit or publish them.
"""
from __future__ import annotations

import argparse
import re
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots    # noqa: E402
import dgb_shots   # noqa: E402

SETTLE = 200
LIST_SETTLE = 30


def _crc(png: Path) -> int:
    from PIL import Image
    return zlib.crc32(Image.open(png).convert("RGB").tobytes())


def run(a, core_mod, image_mod) -> int:
    from PIL import Image, ImageDraw
    a.out.mkdir(parents=True, exist_ok=True)
    s = gb_shots.Session(core_mod, image_mod, a.image, a.out, a.tag)
    dgb_shots.boot_to_gb_session(s, a.image, which=a.which)
    shots: list[tuple[str, Path]] = []
    last = None
    for i, tok in enumerate(a.keys):
        if tok.startswith("shot:"):
            body = tok[5:]
            name, _, claims = body.partition("=")
            claim = [c for c in claims.split("|") if c] or None
            p = s.shot(f"{len(shots):02d}_{name}", f"{a.tag} {name}", allow_same=True, claim=claim)
            shots.append((name + (f"  [reads: {claims}]" if claims else ""), p))
            print(f"  shot {name:14s} crc={_crc(p):08x} {('claims ' + claims + ' OK') if claims else ''}")
            continue
        if tok.startswith("wait:"):
            s.run(int(tok[5:]))
            print(f"  wait {tok[5:]} frames")
            continue
        m = re.fullmatch(r"([A-Z]+)(?:\*(\d+))?(?::(\d+))?", tok)
        if not m:
            raise SystemExit(f"bad token {tok!r}")
        key, n, settle = m.group(1), m.group(2), m.group(3)
        cnt = int(n) if n else 1
        st = int(settle) if settle else (LIST_SETTLE if n else SETTLE)
        for _ in range(cnt):
            s.tap(key, settle=st)
        p = s.shot(f"tap{i:02d}_{tok.replace('*', 'x').replace(':', '_')}", f"{a.tag} after {tok}",
                   allow_same=True)
        c = _crc(p)
        print(f"  tap {i:2d} {tok:12s} crc={c:08x} {'CHANGED' if c != last else 'same'}")
        last = c
    W, H, PAD, CAP = 240 * 2, 160 * 2, 4, 14
    cols = 2
    rows = (len(shots) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * (W + PAD) + PAD, max(rows, 1) * (H + CAP + PAD) + PAD), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for k, (name, p) in enumerate(shots):
        im = Image.open(p).convert("RGB").resize((W, H), Image.NEAREST)
        x, y = PAD + (k % cols) * (W + PAD), PAD + (k // cols) * (H + CAP + PAD)
        sheet.paste(im, (x, y + CAP))
        d.text((x + 2, y + 1), f"{a.tag}: {name}", fill=(255, 255, 255))
    sheet.save(a.out / "sheet.png")
    print(f"sheet: {a.out / 'sheet.png'} ({len(shots)} shots)")
    return 1 if getattr(s, "any_claim_failed", False) else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--which", required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--tag", default="y3")
    ap.add_argument("image", type=Path)
    raw = sys.argv[1:]
    i = raw.index("--")
    a = ap.parse_args(raw[:i])
    a.keys = raw[i + 1:]
    core_mod, image_mod = gb_shots.load_mgba()
    return run(a, core_mod, image_mod)


if __name__ == "__main__":
    sys.exit(main())
