#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""dex_drop_probe.py -- BACKLOG #296 regression probe: dropped DOWN presses in the dex type view.

One fused delta-artless image (fuse_rom.py Emerald + fuse_sav.py Emerald.sav), the y18 g3-dex
flow (nav row 6, L to the type view, two shot-equivalent 96-frame runs), then N DOWN presses
per settle value, each press's header-line change checked.  Prints the dropped press numbers
per settle and a TOTAL; exit 1 when TOTAL > 0.

    /usr/local/bin/python3 tools/dex_drop_probe.py IMAGE.gba [N=50] [settle,settle,...]

History: before the #296 slice fix the artless image dropped 29 presses over settles
50..130 step 3 (N=50); after it, 0.  A press is lost when its 3 hold frames sit wholly inside
one uninterrupted repaint (no key_poll) -- see pdna_pick.c DEX_BOB_SLICE.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gb_shots  # noqa: E402
from y18_parity import header_px  # noqa: E402


def main(argv: "list[str]") -> int:
    img = Path(argv[1])
    n = int(argv[2]) if len(argv) > 2 else 50
    settles = [int(x) for x in argv[3].split(",")] if len(argv) > 3 else list(range(50, 131, 3))
    core_mod, image_mod = gb_shots.load_mgba()
    total = 0
    for settle in settles:
        s = gb_shots.Session(core_mod, image_mod, img, Path("/tmp/dex_drop_probe"), "dd_")
        s.tap("START", settle=gb_shots.BIG_SETTLE)
        for _ in range(6):
            s.tap("DOWN")
        s.tap("A", settle=gb_shots.BIG_SETTLE)
        s.run(240)
        s.run(200)
        s.run(96)
        s.tap("L", settle=gb_shots.BIG_SETTLE)
        s.run(200)
        s.run(96)
        drops = []
        for i in range(n):
            before = header_px(s)
            s.tap("DOWN", settle=settle)
            if header_px(s) == before:
                drops.append(i + 1)
        total += len(drops)
        print(f"settle {settle:3d} drops {drops}", flush=True)
    print(f"TOTAL drops {total}")
    return 1 if total else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
