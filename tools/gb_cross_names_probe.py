#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""gb_cross_names_probe.py -- BACKLOG #310 evidence: what does the journal call each Game Boy CROSSING drop?

Each leg does the crossing drop in a Game Boy session (delta-artless fused with Red / Gold, fresh --vsd-style
virtual SD from the template), then SELECT+L (undo) and prints nothing -- the frame `05_undo_chord` /
`05_up_undo` shows the floor refusal "Can't undo <step name>. It crossed into another file", which names the
step.  Observed on main d9504f0 + z3 (2026-10-01):
    UP  (GB -> Bank, last slot of Red box 1)      -> "Transfer up"
    DOWN exa (native Gen-2 Bank cell -> Gold)     -> "Transfer down"
    DOWN brg (native Gen-1 Bank cell -> Gold)     -> "Transfer down"
    DOWN g3  (plain Gen-3 Bank cell -> Gold/Red)  -> "Transfer down" (was "Paste" before the #310 fix: gb_persist("bank-down") now)
No leg says "Box move".  Needs /tmp/host_vsdimg_test_tmpl (run tests/run_host_tests.py once).

    /usr/local/bin/python3 tools/gb_cross_names_probe.py RED.gba GOLD.gba OUTDIR     (fuse_gb.py images)
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dgb_shots  # noqa: E402
import gb_shots  # noqa: E402

K = gb_shots.KEY


def chord_undo(s, tag):
    def strip():
        return np.asarray(s.screen.to_pil().convert("RGB"), dtype=np.int16)[150:160, 78:240]
    s.run(170)
    pre = strip()
    s.core.set_keys(raw=K["SEL"]); s.run(8)
    s.core.set_keys(raw=K["SEL"] | K["L"]); s.run(gb_shots.HOLD)
    s.core.set_keys(raw=K["SEL"]); s.run(6); s.core.set_keys(raw=0)
    for _ in range(0, 700, 3):
        s.run(3)
        if int((np.abs(strip() - pre).max(axis=2) > 0).sum()) > 0:
            break
    s.run(45); s._io_quiesce()
    s.shot(tag, "SELECT+L: the floor refusal names the crossed step", allow_same=True)


def fresh_session(core_mod, image_mod, rom, out, prefix, name):
    img = Path(f"/tmp/gb_cross_names_{name}.img")
    dgb_shots._vsd_mkimg_from_template(img)
    gb_shots.set_default_vsd(img)
    try:
        s = gb_shots.Session(core_mod, image_mod, Path(rom), out, prefix)
    finally:
        gb_shots.set_default_vsd(None)
    s.run(700); s.run(100)
    return s


def leg_up(core_mod, image_mod, red, out):
    s = fresh_session(core_mod, image_mod, red, out, "up_", "up")
    T = lambda k, n=1, **kw: s.press_n(k, n, **kw)
    T("DOWN", 3, settle=100); T("RIGHT", 4, settle=100)       # the LAST slot: a slot-0 release is too big to record (#307)
    T("SEL", settle=100); T("A", settle=150)
    T("UP", 3, settle=100); T("UP", settle=150); T("UP", settle=150); s.run(100)
    for _ in range(3):                                        # drop, origin prompt RED, wall, notice
        T("A", settle=40); s.run(300)
    T("A", settle=40); s.run(300)
    T("DOWN", 2, settle=150); s.run(100)
    chord_undo(s, "05_up_undo")


def leg_down(core_mod, image_mod, rom, out, tag, bank_r, slot, mv, boxes_r, upn):
    s = fresh_session(core_mod, image_mod, rom, out, f"d_{tag}_", tag)
    T = lambda k, n=1, **kw: s.press_n(k, n, **kw)
    T("UP", 3, settle=100); T("UP", upn, settle=60)
    if bank_r: T("R", bank_r, settle=150)
    if slot: T("RIGHT", slot, settle=100)
    T("A", settle=150); T("DOWN", mv, settle=60); T("A", settle=150)
    T("DOWN", 5, settle=150)
    for _ in range(boxes_r): s.tap("R", settle=150)
    for _ in range(6):
        T("A", settle=500)
        if dgb_shots.gb_claims.find(s.screen.to_pil().convert("RGB"), "in-session only"):
            break
    T("A", settle=400)
    chord_undo(s, "05_undo_chord")


def main() -> int:
    red, gold, out = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    out.mkdir(parents=True, exist_ok=True)
    core_mod, image_mod = gb_shots.load_mgba()
    leg_up(core_mod, image_mod, red, out)
    leg_down(core_mod, image_mod, red, out, "rg3", 2, 0, 3, 4, 4)
    leg_down(core_mod, image_mod, gold, out, "g3", 2, 0, 3, 13, 6)
    leg_down(core_mod, image_mod, gold, out, "exa", 0, 0, 2, 13, 6)
    leg_down(core_mod, image_mod, gold, out, "brg", 0, 1, 2, 13, 6)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
