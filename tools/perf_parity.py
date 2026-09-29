#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""perf_parity.py — BACKLOG #73 speed-parity harness: drives the SAME navigation
through two headless mGBA sessions (a full-art image and an artless image) and
compares the app's OWN PDNA_PERF span/rollup telemetry (source/perf.{c,h}) —
not a frame-count proxy, not a pixel-diff timer. The audit that opened #73 found
its own proxy (settle frames) was measuring 1-7 frame transitions against a
harness that itself settles 40-180 frames per tap, i.e. noise. This tool reads
the app's own millisecond clock instead (perf_ticks()/perf_ms(), a real hardware
timer), same as the number Guy would see in a real /PokeDNA/log.txt off actual
hardware.

WHAT IT DRIVES (one navigation script, run against both images)
-----------------------------------------------------------------
From the box screen (boots straight there — see fuse recipe below): bank era
paint, party strip enter, dex open, trainer card, bag, map open, day-care —
each is its OWN perf_span_begin/end pair (source/pdna_box.c, pdna_pick.c,
pdna_main.c, and — added for this backlog, since these four had none before —
source/pdna_trainer.c, pdna_bag.c, pdna_map.c, pdna_main.c's pdna_daycare()).
Box-grid itself is measured by LEAVING to another screen and returning B —
every return-to-box re-enters pdna_box() and opens a fresh "box"/"bank" span
(the very first-ever box paint right after boot closes view_save()'s own
"boot" span instead, which is a different, noisier number — SD-open + Gen-3
parse + first paint all at once — so it is reported separately, not folded
into the "box" row).

Summary "enter" is PERF_REP_MON ("summary.open"), a rollup, not a span — it
only flushes on LEAVING the summary (perf_rep_flush), and it only covers the
FIRST paint (the portrait card). Flipping to the moves page (R) is a plain
repaint with no span of its own in this tree; this tool does not invent one
(BACKLOG #73's own scope note: additive spans only where a screen had NONE).
That gap is called out in the generated report, not hidden.

GB (Red) box grid needs the multi-corpus `make delta-gb` image (Makefile) — a
SEPARATE pair of fused images from the plain Gen-3 one, since that image boots
into a "PICK A SAVE" picker first. The picker's own boot-picker interaction
plus the very first pick closes the SAME stale view_save "boot" span (it was
never closed by the Gen-3 path, because no Gen-3 save was ever opened this
session) — so the first Red pick is thrown away and only the 2nd..(N+1)th
picks (DOWN, A, A -> box grid; B -> back to the picker) are counted, the same
"discard the boot-contaminated first sample" rule as the plain image.

USAGE
-----
    /usr/local/bin/python3 tools/perf_parity.py \\
        --normal pokedna-delta-fused.gba --artless pokedna-delta-artless-fused.gba \\
        [--normal-gb pokedna-delta-gb.gba --artless-gb pokedna-delta-artless-gb.gba] \\
        --runs 3

Needs the SAME libmgba-py vendor tree every headless-mGBA tool in this repo
uses (rec2mp4's vendored build) and its interpreter — MACHINE-SPECIFIC, same
as the Makefile's own retail-gate target: /usr/local/bin/python3, vendor path
hardcoded below. On another machine, edit VENDOR_DIR or pass --vendor.

Prints one table per image (median + spread over --runs, "spread" = max-min
in ms) and a normal-vs-artless ratio column; exits 0 always (this is a report
tool, not a gate) unless the images/vendor cannot be loaded at all.
"""
from __future__ import annotations

import argparse
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from gb_shots import Session, HOLD, SETTLE, BIG_SETTLE  # noqa: E402 (sibling import, gb_shots.py untouched)

# MACHINE-SPECIFIC, matching the Makefile's retail-gate target -- see this file's own
# docstring. Overridable with --vendor for a different checkout.
VENDOR_DIR_DEFAULT = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/projects/rec2mp4/vendor"

FLUSH_SETTLE = 150   # frames run after an action so perf's 1500ms rate-limited flush lands
                      # (150 frames @ ~59.7 fps ~= 2.5s of emulated time -- comfortably over
                      # perf.c's PERF_FLUSH_GAP_MS)


def load_mgba_capturing(vendor_dir: str):
    """Same load as gb_shots.load_mgba(), except it installs a CAPTURING logger instead
    of mgba.log.silence() -- this tool's whole point is reading the log, not hiding it.
    Returns (core_mod, image_mod, lines) where `lines` is a list every log message (any
    category) gets appended to, decoded from the cffi cdata char* the mGBA Python
    bindings hand back."""
    vendor = Path(vendor_dir)
    if not vendor.is_dir():
        sys.exit(f"vendor path missing: {vendor} (see this file's docstring / --vendor)")
    sys.path.insert(0, str(vendor))
    import mgba.core, mgba.image, mgba.log  # noqa: E402
    from mgba.log import ffi               # noqa: E402

    lines: list[str] = []

    class Capture(mgba.log.Logger):
        def log(self, category, level, message):  # noqa: A002 (shadows builtin, matches base API)
            try:
                m = ffi.string(message).decode("utf-8", "replace")
            except TypeError:
                m = str(message)
            lines.append(m)

    mgba.log.install_default(Capture())
    return mgba.core, mgba.image, lines


def new_lines(lines: list[str], since: int) -> list[str]:
    return [l for l in lines[since:] if l.startswith("perf ")]


def parse_span_ms(line: str, name: str) -> int | None:
    """'perf NAME: N ms, sd ...' -- the span-close line perf_span_end() prints."""
    prefix = f"perf {name}: "
    if not line.startswith(prefix):
        return None
    rest = line[len(prefix):]
    ms = rest.split(" ms", 1)[0]
    return int(ms)


def parse_rep_ms(line: str, name: str) -> int | None:
    """'perf NAME xN: tot M ms, worst W ms, ...' -- the rollup-flush line perf_rep_flush()
    prints. With exactly one occurrence (xN==x1, this tool's own usage) tot==worst==the
    enter cost; this returns `tot` either way, since xN is always 1 here (enter, then
    immediately leave)."""
    prefix = f"perf {name} x"
    if not line.startswith(prefix):
        return None
    tot = line.split("tot ", 1)[1].split(" ms", 1)[0]
    return int(tot)


def find_last(lines: list[str], pred) -> int | None:
    for l in reversed(lines):
        v = pred(l)
        if v is not None:
            return v
    return None


class Driver:
    """One booted Session plus the screen-enter primitives this backlog item needs."""

    def __init__(self, core_mod, image_mod, lines: list[str], rom: Path, prefix: str):
        self.lines = lines
        self.s = Session(core_mod, image_mod, rom, Path("/tmp"), prefix)  # out_dir unused (no shots)

    def _settled(self, n0: int) -> list[str]:
        self.s.run(FLUSH_SETTLE)
        return new_lines(self.lines, n0)

    def to_box(self) -> int | None:
        """From any subscreen: B back to the box, return the fresh 'box'/'bank' span ms
        (whichever this box call turns out to be -- see nav() below for which one a given
        caller expects)."""
        n0 = len(self.lines)
        self.s.tap("B", settle=BIG_SETTLE)
        got = self._settled(n0)
        return find_last(got, lambda l: parse_span_ms(l, "box"))

    def nav_span(self, downs: int, name: str) -> tuple[int | None, int | None]:
        """START -> `downs` DOWN taps -> A -> read back the named span's ms, then B home
        and read back THAT return's own 'box'/'bank' span too -- every one of these six
        items fully exits pdna_box() to a distinct screen and a distinct outer-loop call
        re-enters it on return, so each B-home is an independent, clean box-grid sample
        (see this file's own module docstring for why the party strip, below, is NOT
        one of these)."""
        n0 = len(self.lines)
        self.s.tap("START", settle=BIG_SETTLE)
        for _ in range(downs):
            self.s.tap("DOWN", settle=SETTLE)
        self.s.tap("A", settle=BIG_SETTLE)
        got = self._settled(n0)
        ms = find_last(got, lambda l: parse_span_ms(l, name))
        box_ms = self.to_box()
        return ms, box_ms

    def party_strip(self) -> int | None:
        """START -> A (Party, index 0). For a save WITH PC storage (every corpus save
        used here) this does NOT open the standalone party_list() screen that owns the
        "party" span (source/pdna_main.c ~3852) -- it arms app_box_start_set(3) and lets
        pdna_box()'s own unconditional call open straight onto the party-strip OVERLAY
        (pcp_open_party_strip, source/pdna_box.c), which paints ON TOP of the box grid
        pdna_box() just finished painting. The box grid's OWN "box"/"bank" span has
        already closed (perf_first_paint is a one-shot local, consumed by that first
        paint) by the time the strip opens, so this measures the box-grid repaint that
        precedes the strip, not the strip itself -- there is no dedicated span for the
        strip's own additional draw cost. Returned here for completeness; the report
        does NOT count this as an independent 'party strip enter' number (it IS a
        box-grid sample, see the module docstring)."""
        n0 = len(self.lines)
        self.s.tap("START", settle=BIG_SETTLE)
        self.s.tap("A", settle=BIG_SETTLE)
        got = self._settled(n0)
        ms = find_last(got, lambda l: parse_span_ms(l, "box")) or find_last(got, lambda l: parse_span_ms(l, "bank"))
        self.s.tap("B", settle=BIG_SETTLE)          # close the strip -> back to the grid
        self.s.run(FLUSH_SETTLE)
        return ms

    def summary_enter(self) -> int | None:
        """From the box grid: A on the cursor's cell (must be occupied -- box index 0 in
        every corpus save used here has a mon), A on VIEW/EDIT (already selected) opens
        the summary (portrait, card 0); B leaves, flushing PERF_REP_MON's 'summary.open'
        rollup. Two more B's return to the box grid."""
        n0 = len(self.lines)
        self.s.tap("A", settle=BIG_SETTLE)          # cursor cell -> mon menu
        self.s.tap("A", settle=BIG_SETTLE)          # VIEW/EDIT -> summary (portrait, card 0)
        self.s.tap("B", settle=BIG_SETTLE)          # leave -> summary.open rollup flushes
        got = self._settled(n0)
        ms = find_last(got, lambda l: parse_rep_ms(l, "summary.open"))
        self.s.tap("B", settle=BIG_SETTLE)          # mon menu -> box grid (defensive: no-op if already there)
        self.s.run(FLUSH_SETTLE)
        return ms


# (downs, span name, row label) -- enum order in source/pdna_layout.h PDNA_NAV_ITEMS
NAV_SCREENS = [
    (1, "bank", "bank era paint"),
    (6, "dex", "dex open"),
    (3, "trainer", "trainer card"),
    (7, "bag", "bag"),
    (15, "map", "map open"),
    (2, "daycare", "day-care"),
]


def measure_image(core_mod, image_mod, lines: list[str], rom: Path, prefix: str,
                   runs: int) -> dict[str, list[int]]:
    d = Driver(core_mod, image_mod, lines, rom, prefix)
    results: dict[str, list[int]] = {label: [] for _, _, label in NAV_SCREENS}
    results["box-grid first paint (return-to-box)"] = []
    results["summary enter (portrait)"] = []

    for i in range(runs):
        for downs, name, label in NAV_SCREENS:
            ms, box_ms = d.nav_span(downs, name)
            if ms is not None:
                results[label].append(ms)
            if box_ms is not None:
                results["box-grid first paint (return-to-box)"].append(box_ms)
            print(f"  [{prefix}{i+1}/{runs}] {label:32s} -> {ms} ms   (return-to-box: {box_ms} ms)")

        summary_ms = d.summary_enter()
        if summary_ms is not None:
            results["summary enter (portrait)"].append(summary_ms)
        print(f"  [{prefix}{i+1}/{runs}] {'summary enter (portrait)':32s} -> {summary_ms} ms")

    return results


def measure_gb_box(core_mod, image_mod, lines: list[str], rom: Path, prefix: str,
                    runs: int) -> list[int]:
    """The multi-corpus delta-gb image: boot -> PICK A SAVE -> DOWN (Red) -> A (box grid,
    #279: no info page). First pick's paint closes the STALE view_save 'boot' span (no Gen-3
    save opened this session) -- discarded. Runs+1 total picks; the last `runs` are
    'perf box:'/'perf bank:' lines -- a Game Boy BoxSource's own `capacity` field puts
    it through the SAME is_bank-named span pdna_box.c uses for the Gen-3 Bank (screen
    shows a "(BANK)"/"SAVE" tab pair, confirmed by screenshot during development), so
    both names are accepted here."""
    d = Driver(core_mod, image_mod, lines, rom, prefix)
    # The multi-corpus image's boot stall (an "Opening save... N/13 read file" screen,
    # presumably an SD-scan retry loop with no SD hardware to answer it in the emulator)
    # runs WELL past Session()'s own 180-frame boot settle -- measured empirically at
    # ~1200 frames before the "PICK A SAVE" picker appears. Wait it out explicitly so
    # the DOWN/A/A below land on the picker, not mid-stall.
    d.s.run(1200)
    out: list[int] = []
    for i in range(runs + 1):
        n0 = len(lines)
        d.s.tap("DOWN", settle=BIG_SETTLE)   # picker resets to row 0 every time it's shown
        d.s.tap("A", settle=BIG_SETTLE)      # Red row -> box grid (#279: no info page in between)
        d.s.run(FLUSH_SETTLE)
        got = new_lines(lines, n0)
        box_ms = find_last(got, lambda l: parse_span_ms(l, "box"))
        if box_ms is None:
            box_ms = find_last(got, lambda l: parse_span_ms(l, "bank"))
        if i == 0:
            print(f"  [{prefix}] first pick (boot-contaminated, discarded): "
                  f"{[l for l in got if l.startswith('perf boot')]}")
            d.s.tap("B", settle=BIG_SETTLE)  # box -> back to picker
            continue
        if box_ms is not None:
            out.append(box_ms)
        print(f"  [{prefix}{i}/{runs}] {'GB (Red) box grid':32s} -> {box_ms} ms")
        d.s.tap("B", settle=BIG_SETTLE)      # box -> back to picker
    return out


def summarize(vals: list[int]) -> str:
    if not vals:
        return "n/a"
    med = statistics.median(vals)
    spread = max(vals) - min(vals)
    return f"{med:.0f} ms (spread {spread}, n={len(vals)}: {vals})"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--normal", type=Path, required=True,
                     help="full-art delta image, ROM+save fused (tools/fuse_rom.py then "
                          "tools/fuse_sav.py, no --gb) -- boots straight to the box screen")
    ap.add_argument("--artless", type=Path, required=True, help="same, but the artless delta build")
    ap.add_argument("--normal-gb", type=Path, default=None,
                     help="optional: `make delta-gb` full-art image (Red/Gold/Crystal + "
                          "Emerald.sav) for the GB (Red) box-grid row")
    ap.add_argument("--artless-gb", type=Path, default=None, help="same, artless")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--vendor", default=VENDOR_DIR_DEFAULT, help="libmgba-py vendor dir")
    a = ap.parse_args(argv)

    for p in (a.normal, a.artless):
        if not p.is_file():
            sys.exit(f"{p}: not a file")

    core_mod, image_mod, lines = load_mgba_capturing(a.vendor)

    print(f"== normal: {a.normal.name} ==")
    normal = measure_image(core_mod, image_mod, lines, a.normal, "n_", a.runs)
    print(f"\n== artless: {a.artless.name} ==")
    artless = measure_image(core_mod, image_mod, lines, a.artless, "a_", a.runs)

    gb_normal = gb_artless = None
    if a.normal_gb and a.artless_gb:
        print(f"\n== normal GB: {a.normal_gb.name} ==")
        gb_normal = measure_gb_box(core_mod, image_mod, lines, a.normal_gb, "ngb_", a.runs)
        print(f"\n== artless GB: {a.artless_gb.name} ==")
        gb_artless = measure_gb_box(core_mod, image_mod, lines, a.artless_gb, "agb_", a.runs)

    print("\n\n=== BACKLOG #73 speed-parity table (median of {} runs) ===".format(a.runs))
    rows = list(normal.keys())
    print(f"{'screen':38s} {'normal':>26s} {'artless':>26s} {'ratio':>8s}  slower?")
    for row in rows:
        nv, av = normal[row], artless[row]
        nmed = statistics.median(nv) if nv else None
        amed = statistics.median(av) if av else None
        ratio = (amed / nmed) if (nmed and amed and nmed > 0) else None
        flag = "SLOWER >1.5x" if (ratio and ratio > 1.5) else ""
        print(f"{row:38s} {summarize(nv):>26s} {summarize(av):>26s} "
              f"{(f'{ratio:.2f}x' if ratio else 'n/a'):>8s}  {flag}")
    if gb_normal is not None:
        nmed = statistics.median(gb_normal) if gb_normal else None
        amed = statistics.median(gb_artless) if gb_artless else None
        ratio = (amed / nmed) if (nmed and amed and nmed > 0) else None
        flag = "SLOWER >1.5x" if (ratio and ratio > 1.5) else ""
        print(f"{'GB (Red) box grid':38s} {summarize(gb_normal):>26s} "
              f"{summarize(gb_artless):>26s} {(f'{ratio:.2f}x' if ratio else 'n/a'):>8s}  {flag}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
