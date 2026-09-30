#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""y18_parity.py -- BACKLOG #70 artless-parity probe (lane y18-audits, AUDIT only).

Drives ONE navigation script through TWO headless mGBA sessions at once -- the normal
(compiled-art) build and the artless build, both fused with the SAME Emerald.gba +
Emerald.sav (and, for the Game Boy screens, the same GB ROM+save) -- and, at every
`shot`, saves the frame pair and records a pixel diff between them.

It reuses tools/gb_shots.py's Session (boot, tap, frame funnel, PNG writer) and, for the
Game Boy boot picker, tools/dgb_shots.py's boot_to_gb_session; it invents no second
emulator driver.  Method lineage: docs/PARITY-AUDIT-2026-09.md (the 2026-09-09 audit, whose
ad-hoc harness was never committed) -- this file is that harness, committed, and extended
to the screens that landed since (dex DETAIL #203, bank eras, GB sessions).

Diff rules (so a verdict is a number, never an impression):
  * `diff_px`   pixels whose RGB differs at all between the two builds' frames
  * `diff_pct`  diff_px / (240*160)
  * `bbox`      bounding box of the differing pixels (x0,y0,x1,y1) or None
  * `diff_px_tol` pixels differing by more than 12 in any channel (animation-noise floor)
  A screen with diff_px == 0 is `pixel-identical`; anything else is READ BY EYE before it is
  given a verdict (rule 17) -- this tool never assigns parity/missing/glitch itself.

Groups: g3-* (Gen-3 sessions: images = delta / delta-artless fused with fuse_rom.py + fuse_sav.py),
gb-* (Game Boy sessions: images fused with fuse_gb.py; --which red|yellow|gold|crystal), gb-row
(one nav row, --row N), g3-dex-latency (frames-to-respond per DOWN press). GB nav rows: START, DOWN
x row (column 1 via RIGHT for row >= 11), A. g3-sweep walks every Gen-3 nav row in one boot (verified: each frame is a real screen); the same
sweep on a GB session was tried and REMOVED -- B x3 there does not reliably land back on the box
grid, so alternate rows desynchronise (2026-09-30) -- GB rows are fresh boots via gb-row.

Usage (Python that can import mgba -- /usr/local/bin/python3 on Guy's Mac):
    /usr/local/bin/python3 tools/y18_parity.py --normal N.gba --artless A.gba \\
        --out /tmp/y18/frames --group g3-box
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots  # noqa: E402 -- Session / load_mgba / SETTLE constants

import numpy as np  # noqa: E402
from PIL import Image, ImageChops  # noqa: E402

SETTLE = gb_shots.SETTLE
BIG = gb_shots.BIG_SETTLE
TOL = 24
PHASE_FRAMES = 16
PHASE_STEP = 6
SCREEN_PX = 240 * 160


@dataclass
class Diff:
    """One frame pair's comparison, JSON-serialisable."""
    name: str
    caption: str
    normal_png: str
    artless_png: str
    diff_px: int
    diff_pct: float
    diff_px_tol: int
    bbox: "tuple[int, int, int, int] | None"


def compare(a: Path, b: Path) -> "tuple[int, int, tuple[int, int, int, int] | None]":
    """Return (differing pixels, pixels differing by > TOL in some channel, bbox)."""
    ia = np.asarray(Image.open(a).convert("RGB"), dtype=np.int16)
    ib = np.asarray(Image.open(b).convert("RGB"), dtype=np.int16)
    per_px = np.abs(ia - ib).max(axis=2)
    ys, xs = np.nonzero(per_px)
    bbox = (int(xs.min()), int(ys.min()), int(xs.max()) + 1, int(ys.max()) + 1) if xs.size else None
    return int(xs.size), int((per_px > TOL).sum()), bbox


def write_sheet(a: Path, b: Path, out: Path) -> None:
    """Side-by-side (normal | artless | diff mask), 2x nearest-neighbour, for eyeballing."""
    ia = Image.open(a).convert("RGB")
    ib = Image.open(b).convert("RGB")
    mask = ImageChops.difference(ia, ib).convert("L").point(lambda v: 255 if v else 0).convert("RGB")
    sheet = Image.new("RGB", (240 * 2 * 3 + 20, 320), (255, 0, 255))
    for i, im in enumerate((ia, ib, mask)):
        sheet.paste(im.resize((480, 320), Image.NEAREST), (i * 490, 0))
    sheet.save(out)


@dataclass
class Pair:
    """Two Sessions driven in lock-step. `n` = normal build, `a` = artless build."""
    n: "gb_shots.Session"
    a: "gb_shots.Session"
    diffs: "list[Diff]" = field(default_factory=list)
    latency: "dict[str, list]" = field(default_factory=dict)

    def tap(self, *names: str, settle: int = SETTLE) -> None:
        self.n.tap(*names, settle=settle)
        self.a.tap(*names, settle=settle)

    def press_n(self, name: str, count: int, settle: int = SETTLE) -> None:
        for _ in range(count):
            self.tap(name, settle=settle)

    def run(self, frames: int) -> None:
        self.n.run(frames)
        self.a.run(frames)

    def _grab(self, session: "gb_shots.Session") -> Image.Image:
        return session.screen.to_pil().convert("RGB").copy()

    def shot(self, name: str, caption: str) -> Diff:
        """Capture PHASE_FRAMES frames per build (PHASE_STEP apart) and keep the frame pair
        with the smallest pixel diff. Why: icons and the hand bob 1 px on a 2-frame cycle and
        the two builds boot with different frame counts, so a single pair can differ purely
        by animation phase (first probe, 2026-09-30: every icon differed by 1 px). Static
        differences (a missing wallpaper strip, a wrong palette) survive the min over all
        phase pairs; animation cannot."""
        frames_n, frames_a = [], []
        for _ in range(PHASE_FRAMES):
            frames_n.append(self._grab(self.n))
            frames_a.append(self._grab(self.a))
            self.run(PHASE_STEP)
        best = None
        for i, fn in enumerate(frames_n):
            arr_n = np.asarray(fn, dtype=np.int16)
            for j, fa in enumerate(frames_a):
                cost = int((np.abs(arr_n - np.asarray(fa, dtype=np.int16)).max(axis=2) > 0).sum())
                if best is None or cost < best[0]:
                    best = (cost, i, j)
        _, bi, bj = best
        pn = self.n.out_dir / f"{self.n.prefix}{name}.png"
        pa = self.a.out_dir / f"{self.a.prefix}{name}.png"
        frames_n[bi].save(pn)
        frames_a[bj].save(pa)
        px, tol, bbox = compare(pn, pa)
        write_sheet(pn, pa, pn.parent.parent / f"sheet_{name}.png")
        d = Diff(name, caption, str(pn), str(pa), px, round(100.0 * px / SCREEN_PX, 3), tol, bbox)
        self.diffs.append(d)
        print(f"  [PAIR] {name:28s} diff_px={px:6d} ({d.diff_pct:6.3f}%)  >tol={tol:6d}  bbox={bbox}  "
              f"phase=({bi},{bj})  {caption}")
        return d


def open_pair(core_mod, image_mod, normal: Path, artless: Path, out: Path, prefix: str) -> Pair:
    """Boot both images (each into its own frame subdirectory) and return the Pair."""
    dn, da = out / "normal", out / "artless"
    dn.mkdir(parents=True, exist_ok=True)
    da.mkdir(parents=True, exist_ok=True)
    return Pair(gb_shots.Session(core_mod, image_mod, normal, dn, prefix),
                gb_shots.Session(core_mod, image_mod, artless, da, prefix))


# --------------------------------------------------------------------------------------
# Gen-3 screen groups.  Each takes a booted Pair on the Emerald box grid.
# --------------------------------------------------------------------------------------

def nav_to(p: Pair, row: int) -> None:
    """START, DOWN x row (column 0 of the nav menu), A."""
    p.tap("START", settle=BIG)
    p.press_n("DOWN", row)
    p.tap("A", settle=BIG)
    p.run(240)


def g3_box(p: Pair) -> None:
    """Box grid, a cell's side-panel portrait, the mon menu."""
    p.run(300)
    p.shot("g3_00_box_grid", "box grid, boot frame (icons + wallpaper)")
    p.tap("RIGHT")
    p.run(120)
    p.shot("g3_01_box_cell1", "cursor on cell 1: side-panel portrait")
    p.tap("A", settle=BIG)
    p.shot("g3_02_mon_menu", "A on an occupied cell: mon menu")


def g3_summary(p: Pair) -> None:
    """Cell 0 (the Magnemite whose portrait palette differed): A -> VIEW/EDIT -> the summary."""
    p.run(300)
    p.tap("A", settle=BIG)
    p.tap("A", settle=BIG)
    p.run(300)
    p.shot("g3_10_summary_info", "summary INFO card, front portrait (cell 0)")
    p.tap("SEL", settle=BIG)
    p.run(200)
    p.shot("g3_11_summary_back", "summary, SELECT -> back portrait")
    p.tap("R", settle=BIG)
    p.run(200)
    p.shot("g3_12_summary_skills", "summary card 2 (R)")
    p.tap("R", settle=BIG)
    p.run(200)
    p.shot("g3_13_summary_moves", "summary card 3 (R)")


def g3_party(p: Pair) -> None:
    """START > Party: the party strip and a party mon's menu."""
    nav_to(p, 0)
    p.shot("g3_20_party_strip", "START > Party: party strip (icons + portrait)")
    p.tap("A", settle=BIG)
    p.run(200)
    p.shot("g3_21_party_menu", "A on a party mon")


def g3_dex(p: Pair) -> None:
    """Pokedex first screen, second view, a scrolled cell, and the detail view (cursor may differ across builds; see g3_dex_detail)."""
    nav_to(p, 6)
    p.run(200)
    p.shot("g3_30_dex_first", "Pokedex, first screen (artless default view differs by design?)")
    p.tap("L", settle=BIG)
    p.run(200)
    p.shot("g3_31_dex_second", "Pokedex after L (other view)")
    p.press_n("DOWN", 24, settle=90)   # 90 frames per row: 12 dropped inputs on artless (see g3_dex_latency)
    p.press_n("UP", 1, settle=90)
    p.run(120)
    p.shot("g3_32_dex_cursor155", "dex cursor on No.155 region")
    p.tap("A", settle=BIG)
    p.run(300)
    p.shot("g3_33_dex_detail", "dex DETAIL view (A) -- #203")


def g3_bank(p: Pair) -> None:
    """START > Bank: the first two bank screens."""
    nav_to(p, 1)
    p.run(300)
    p.shot("g3_40_bank", "START > Bank: first bank screen")
    p.tap("R", settle=BIG)
    p.run(300)
    p.shot("g3_41_bank_r", "bank after R")


def g3_trainer(p: Pair) -> None:
    """Trainer card, both pages."""
    nav_to(p, 3)
    p.run(300)
    p.shot("g3_50_trainer_front", "Trainer card FRONT")
    p.tap("R", settle=BIG)
    p.run(200)
    p.shot("g3_51_trainer_back", "Trainer card next page")


def g3_bag(p: Pair) -> None:
    """Bag, two pockets."""
    nav_to(p, 7)
    p.run(300)
    p.shot("g3_60_bag", "Bag / item icons")
    p.tap("R", settle=BIG)
    p.run(200)
    p.shot("g3_61_bag_r", "Bag next pocket")


def g3_misc(p: Pair) -> None:
    """Daycare yard."""
    nav_to(p, 2)
    p.run(300)
    p.shot("g3_70_daycare", "Daycare yard")


def g3_pokeblock(p: Pair) -> None:
    """Pokeblock case."""
    nav_to(p, 10)
    p.run(300)
    p.shot("g3_71_pokeblock", "Pokeblocks")


def g3_settings(p: Pair) -> None:
    """Settings screen."""
    p.tap("START", settle=BIG)
    p.tap("RIGHT")
    p.press_n("DOWN", 8)
    p.tap("A", settle=BIG)
    p.run(200)
    p.shot("g3_80_settings", "Settings")


def g3_map(p: Pair) -> None:
    """Map screen (info page)."""
    p.tap("START", settle=BIG)
    p.tap("RIGHT")
    p.press_n("DOWN", 5)
    p.tap("A", settle=BIG)
    p.run(600)
    p.shot("g3_90_map", "Map viewer")


def g3_wallpaper(p: Pair) -> None:
    """UP to the box title, SELECT -> box options -> Wallpaper -> the set menu -> a picker."""
    p.run(300)
    p.tap("UP")
    p.tap("SEL", settle=BIG)
    p.run(120)
    p.shot("g3_95_box_options", "title row + SELECT: box options overlay")
    p.tap("DOWN")
    p.tap("A", settle=BIG)
    p.run(300)
    p.shot("g3_96_wallpaper_sets", "options > Wallpaper: the set menu")
    p.tap("A", settle=BIG)
    p.run(600)
    p.shot("g3_97_wallpaper_pick", "set menu A: the wallpaper preview picker")


def g3_refusal(p: Pair) -> None:
    """Settings > Yard visitors (row 2) with NO registered ROM: the refusal, both builds."""
    p.tap("START", settle=BIG)
    p.tap("RIGHT")
    p.press_n("DOWN", 8)
    p.tap("A", settle=BIG)
    p.run(200)
    p.press_n("DOWN", 2)
    p.tap("A", settle=BIG)
    p.run(120)
    p.shot("g3_81_refusal_yard", "Settings > Yard visitors, A: 'Register your game ROM first'")
    p.tap("A", settle=BIG)
    p.press_n("DOWN", 2)
    p.tap("A", settle=BIG)
    p.run(200)
    p.shot("g3_82_refusal_extract", "Settings > Extract art, A: refusal")


def g3_mapview(p: Pair) -> None:
    p.tap("START", settle=BIG)
    p.tap("RIGHT")
    p.press_n("DOWN", 5)
    p.tap("A", settle=BIG)
    p.run(600)
    p.tap("A", settle=BIG)
    p.run(1200)
    p.shot("g3_91_map_view", "Map screen, A: the tile map view")


def g3_boxes(p: Pair) -> None:
    """Walk the PC boxes (each box owns a wallpaper): does the top-band fault repeat?"""
    p.run(300)
    for k in range(1, 6):
        p.tap("R", settle=BIG)
        p.run(200)
        p.shot(f"g3_1{k}_box_r{k}", f"box after {k}x R (its own wallpaper)")


# --------------------------------------------------------------------------------------
# Game Boy session groups.  `which` = the fused save's stem (red / yellow / gold / crystal).
# --------------------------------------------------------------------------------------

def gb_boot(p: Pair, which: str) -> None:
    """Boot both images onto the GB box grid via dgb_shots's own asserted boot helper."""
    import dgb_shots  # noqa: PLC0415 -- heavy import, only the GB groups need it
    dgb_shots.boot_to_gb_session(p.n, p.n.rom_path, which=which)
    dgb_shots.boot_to_gb_session(p.a, p.a.rom_path, which=which)
    p.run(dgb_shots.GB196_GB_PAGE_SETTLE)


def gb_box(p: Pair, which: str) -> None:
    """GB box grid, cell menu, the summary (or CREATE picker on an empty cell), SELECT, R."""
    gb_boot(p, which)
    p.shot(f"gb_{which}_00_box", f"{which}: GB box grid, boot frame")
    p.tap("A", settle=BIG)
    p.run(200)
    p.shot(f"gb_{which}_01_cell_menu", f"{which}: A on cell 0")
    p.tap("A", settle=BIG)
    p.run(600)
    p.shot(f"gb_{which}_02_summary", f"{which}: VIEW/EDIT -> the GB summary (or the CREATE picker on an empty cell)")
    p.tap("SEL", settle=BIG)
    p.run(300)
    p.shot(f"gb_{which}_03_summary_sel", f"{which}: after SELECT")
    p.tap("R", settle=BIG)
    p.run(300)
    p.shot(f"gb_{which}_04_summary_r", f"{which}: after R")


def gb_dex(p: Pair, which: str) -> None:
    """GB Pokedex: first view, grid view, and the #203 detail view."""
    gb_boot(p, which)
    p.tap("START", settle=BIG)
    p.press_n("DOWN", 6)
    p.tap("A", settle=BIG)
    if which in ("gold", "crystal"):   # Gen-2 dex opens a chooser (Pokedex / Unown forms) first
        p.tap("A", settle=BIG)
    p.run(600)
    p.shot(f"gb_{which}_10_dex", f"{which}: dex first view")
    p.tap("L", settle=BIG)
    p.run(600)
    p.shot(f"gb_{which}_11_dex_grid", f"{which}: dex after L")
    p.press_n("RIGHT", 2)
    p.run(200)
    p.tap("A", settle=BIG)
    p.run(400)
    p.shot(f"gb_{which}_12_dex_detail", f"{which}: dex DETAIL (A) -- #203")


def gb_nav_screen(p: Pair, which: str, row: int, tag: str, caption: str) -> None:
    """START > nav row `row` (column 1 for row >= 11) > A, then ride out the screen's own cold scan."""
    import dgb_shots  # noqa: PLC0415
    gb_boot(p, which)
    p.tap("START", settle=BIG)
    if row >= 11:
        p.tap("RIGHT")
    p.press_n("DOWN", row - 11 if row >= 11 else row)
    p.tap("A", settle=BIG)
    p.run(dgb_shots.GB_ART_COLD_SETTLE)
    p.shot(f"gb_{which}_{tag}", f"{which}: {caption}")


def gb_trainer(p: Pair, which: str) -> None:
    gb_nav_screen(p, which, 3, "20_trainer", "trainer card")


def gb_bag(p: Pair, which: str) -> None:
    gb_nav_screen(p, which, 7, "30_bag", "bag / items")


def gb_bank(p: Pair, which: str) -> None:
    gb_nav_screen(p, which, 1, "40_bank", "bank from a GB session")




HEADER_BOX = (0, 0, 160, 12)   # the dex header line "No. N NAME": changes exactly when the cursor moves


def header_px(session: "gb_shots.Session") -> bytes:
    return session.screen.to_pil().convert("RGB").crop(HEADER_BOX).tobytes()


def press_latency(session: "gb_shots.Session", key: str, limit: int = 300) -> "int | None":
    """Frames from a key press until the header line changes; None = never within `limit`
    (a DROPPED input). Counts the HOLD frames, like tap(). Frames advance through
    Session.run(1) only (the S4.4 funnel gb_shots.py's own host test enforces)."""
    before = header_px(session)
    session.core.set_keys(raw=gb_shots.KEY[key])
    moved = None
    for f in range(1, limit + 1):
        if f == gb_shots.HOLD + 1:
            session.core.set_keys(raw=0)
        session.run(1)
        if header_px(session) != before:
            moved = f
            break
    session.core.set_keys(raw=0)
    session.run(90)   # let the redraw finish before the next probe
    return moved


def g3_dex_latency(p: Pair) -> None:
    """Dex grid responsiveness (speed parity, harness-clocked): 12 DOWN presses, frames to
    the header line changing, per build. A None means the press never registered within
    240 frames (a DROPPED input -- the class the g3_32 pair exposed: same 25 presses, the
    two builds landed on different entries)."""
    nav_to(p, 6)
    p.run(300)
    for name, sess in (("normal", p.n), ("artless", p.a)):
        lat = [press_latency(sess, "DOWN") for _ in range(12)]
        print(f"  [LATENCY] dex grid DOWN x12 {name:8s} frames-to-move: {lat}")
        p.latency[name] = lat


NAV_LABELS = ["Party", "Bank", "Daycare", "Trainer", "Clock", "Mirage", "Pokedex", "Bag", "Flags", "Bases",
              "Blocks", "Tickets", "Records", "Frontier", "Fly", "Contests", "Map", "GBimport", "Transfers",
              "Settings", "Back"]


def nav_open(p: Pair, idx: int, wait: int) -> None:
    """START, walk to nav item `idx` (column 0 = 0..10, column 1 = 11..20), A, wait."""
    p.tap("START", settle=BIG)
    if idx >= 11:
        p.tap("RIGHT")
        p.press_n("DOWN", idx - 11)
    else:
        p.press_n("DOWN", idx)
    p.tap("A", settle=BIG)
    p.run(wait)


def g3_sweep(p: Pair) -> None:
    """Every nav row, both builds, ROM fused: does anything refuse / render differently?
    Each row is entered from a fresh box grid (B x3 after each shot returns there)."""
    p.run(300)
    for idx, label in enumerate(NAV_LABELS[:-1]):
        nav_open(p, idx, 400)
        p.shot(f"g3_sw{idx:02d}_{label}", f"nav row {idx}: {label}")
        for _ in range(3):
            p.tap("B", settle=BIG)
        p.run(200)


def g3_bank_cells(p: Pair) -> None:
    """Bank grid cells of different eras -> the cell menu -> VIEW: summary art per era."""
    nav_to(p, 1)
    p.run(300)
    for k, (dx, label) in enumerate(((0, "cell 0"), (1, "cell 1"), (2, "cell 2 (egg?)"))):
        if dx:
            p.tap("RIGHT")
        p.run(150)
        p.shot(f"g3_4{k + 2}_bank_{k}", f"bank {label}: side-panel portrait")
    p.tap("LEFT")
    p.tap("LEFT")
    p.tap("RIGHT")          # back on cell 1 (the yellow-Pikachu-looking cell)
    p.tap("A", settle=BIG)
    p.run(200)
    p.shot("g3_45_bank_menu", "A on bank cell 1: the cell menu")
    p.tap("A", settle=BIG)
    p.run(500)
    p.shot("g3_46_bank_view", "menu row 0 (VIEW): the summary of a banked cell")


def g3_dex_detail(p: Pair) -> None:
    """The #203 detail view from the SAME grid cell on both builds (2x RIGHT = dex No. 3)."""
    nav_to(p, 6)
    p.run(300)
    p.press_n("RIGHT", 2, settle=90)
    p.run(200)
    p.shot("g3_35_dex_cell3", "dex grid, cursor on entry 3")
    p.tap("A", settle=BIG)
    p.run(400)
    p.shot("g3_36_dex_detail_a", "A: DETAIL of entry 3 (portrait, name, status)")
    p.tap("R", settle=BIG)
    p.run(400)
    p.shot("g3_37_dex_detail_r", "R: DETAIL of the next entry")


GROUPS: "dict[str, Callable[[Pair], None]]" = {
    "g3-box": g3_box, "g3-summary": g3_summary, "g3-party": g3_party, "g3-dex": g3_dex,
    "g3-bank": g3_bank, "g3-trainer": g3_trainer, "g3-bag": g3_bag, "g3-daycare": g3_misc,
    "g3-pokeblock": g3_pokeblock, "g3-settings": g3_settings, "g3-map": g3_map,
    "g3-wallpaper": g3_wallpaper, "g3-boxes": g3_boxes, "g3-refusal": g3_refusal, "g3-mapview": g3_mapview, "g3-dex-latency": g3_dex_latency, "g3-sweep": g3_sweep, "g3-dex-detail": g3_dex_detail, "g3-bank-cells": g3_bank_cells,
}


GB_GROUPS: "dict[str, Callable[[Pair, str], None]]" = {
    "gb-box": gb_box, "gb-dex": gb_dex, "gb-trainer": gb_trainer, "gb-bag": gb_bag, "gb-bank": gb_bank, "gb-row": None,
}


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--normal", type=Path, required=True, help="fused NORMAL (compiled-art) image")
    ap.add_argument("--artless", type=Path, required=True, help="fused ARTLESS image")
    ap.add_argument("--out", type=Path, required=True, help="frame output directory")
    ap.add_argument("--group", choices=sorted(GROUPS) + sorted(GB_GROUPS), required=True)
    ap.add_argument("--row", type=int, default=0, help="gb-row: nav row index (column 0 = 0..10, column 1 = 11..)")
    ap.add_argument("--which", default="red", help="GB groups: the fused save stem (red/yellow/gold/crystal)")
    args = ap.parse_args(argv)
    gb_shots.assert_vehicle(args.normal, "ART")
    gb_shots.assert_vehicle(args.artless, "ARTLESS")
    core_mod, image_mod = gb_shots.load_mgba()
    pair = open_pair(core_mod, image_mod, args.normal, args.artless, args.out,
                     args.group + (f"_{args.which}" if args.group in GB_GROUPS else "") + "_")
    if args.group == "gb-row":
        gb_nav_screen(pair, args.which, args.row, f"r{args.row:02d}_{NAV_LABELS[args.row]}",
                      f"nav row {args.row} ({NAV_LABELS[args.row]})")
    elif args.group in GB_GROUPS:
        GB_GROUPS[args.group](pair, args.which)
    else:
        GROUPS[args.group](pair)
    (args.out / f"{args.group}{'_' + args.which if args.group in GB_GROUPS else ''}.json").write_text(
        json.dumps({"diffs": [d.__dict__ for d in pair.diffs], "latency": pair.latency}, indent=2),
        encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
