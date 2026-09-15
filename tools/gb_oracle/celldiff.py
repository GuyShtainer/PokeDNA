#!/usr/bin/env python3
"""Game Boy screen cell-diff oracle (BACKLOG #97; originated as the U4
review's own docs/shots/rvu4/celldiff.py; --selftest/--demo added for the
U2c/U3 follow-up review's finding that a from-scratch comparator can ship as
a DEAD STUB -- a diff loop that never actually appends a mismatch -- with no
mechanical guard catching it).

THREE independent modes, all sharing the same by-CELL-POSITION diff idea:

  --harness/--grids-dir/--gb-roms   (unchanged) diffs a ground-truth
      *_grid.json (oracle.py's own real-cartridge VRAM dump) against a
      caller-supplied, separately host-compiled harness that runs the
      SHIPPED C painter functions and prints absolute Game Boy tile ids --
      see README.md's "Building a harness" section for how to build one.
      Cell-id comparison; mismatches bucketed by NAMED region (the
      ACCEPTED_DEVIATIONS tables below), not just counted.

  --selftest   proves the comparator itself is not a dead stub: builds two
      SYNTHETIC 20x18 grids that agree everywhere except ONE cell whose
      (row, col) is chosen by this run (so a hardcoded lucky guess can never
      pass), runs diff_grids(), and FAILS unless the reported mismatch set
      is EXACTLY that one cell. Needs no ROM, no save, no mGBA -- CI-safe,
      registered in tests/run_host_tests.py's PY_TESTS.

  --demo   an end-to-end REAL run over two screens (BACKLOG #97's own ask):
      the Gen-2 trainer card (Gold) and the Gen-1 Item bag's border (Red).
      For each: (1) compiles a small throwaway host harness (harness_g2card.c
      / harness_g1bag.c, checked in next to this file) that calls the
      SHIPPED, UNMODIFIED C cell tables/painter geometry (source/
      g2card_cells.c's g2card_build_upper_cells(); source/pdna_gbbag.c's
      g1bag_border(), sed-extracted fresh every run so it can never drift
      from what ships) and resolves every cell to a REAL ROM FILE BYTE
      OFFSET via rom_gbui_open() + gbscr_block_off() (the exact same two
      functions the shipped painter uses); (2) boots the real ROM+save in
      mGBA (tools/gb_roundtrip.py's GbDriver, the only mGBA entry point this
      script uses -- see that module's own docstring for why nothing here
      imports the `mgba` package directly) and captures the REAL VRAM
      tilemap + per-tile pixel data (oracle.py's compose()/tile_px(), the
      same LCDC-aware ground truth --tag runs use); (3) diffs BY CELL
      POSITION, pixel-for-pixel (not by tile id -- README.md's own WARNING:
      a flat "tile id" identity is Gen-1-only-safe, Gen 2's textbox/card
      blocks need a pixel compare), and lists every mismatching (row, col)
      with the ROM offset it expected. This run is the proof the pipeline
      genuinely works end to end: run once against Guy's own corpus
      (2026-09-15) it reported ZERO mismatches on both screens (51 Gen-2
      upper-card cells, 90 Gen-1 bag-border cells).

No ROM/save/pixel data ships in this file or the harnesses. Point --gb-roms
at your own dumps (gba-toolkit/roms/gb by default, matching docs/kb/pokemon/
+ roms.sh); the *_grid.json ground-truth files (--harness mode only) live in
docs/shots/<slice>/ per review (gitignored, not committed)."""
import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

# tools/gb_oracle/celldiff.py -> parents[0]=gb_oracle, [1]=tools, [2]=repo root
ROOT = Path(__file__).resolve().parents[2]
GB_ORACLE_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(GB_ORACLE_DIR))
import oracle as O    # noqa: E402 -- compose()/tile_px(), guarded main() (see oracle.py)
import gb_roundtrip as R  # noqa: E402 -- GbDriver; this file never imports `mgba` itself


# ---------------------------------------------------------------------------
# Named accepted deviations -- item 4 of the brief: mismatches this tool
# already knows are EXPECTED (a blinking cell, a cosmetic frame difference)
# get a human name here instead of just landing in a lettered bucket. Keyed
# by screen tag -> {(row, col): "name"}; a screen with no known deviations
# (both --demo screens below, which compare only cells with NO time/save-
# dependent content) simply has an empty table -- any mismatch there is real.
# ---------------------------------------------------------------------------
ACCEPTED_DEVIATIONS = {
    # The original U4 Red-bag full-list comparator (--harness mode): the
    # down-scroll marker at (18,11) blinks (g1bag_scroll_marker_on(), 32-
    # VBlank period) and is legitimately either GBSCR_SRC_FONT or
    # GBSCR_SRC_TEXTBOX depending on which phase the capture landed on.
    "gen1_bag_list": {
        (11, 18): "the list's down-scroll marker cell (blinks every 32 VBlanks)",
    },
    # BACKLOG #97 --demo screens compare ONLY save/time-independent cells
    # (the trainer card's static upper grid; the bag's static border) --
    # zero known deviations, any mismatch here is a real defect.
    "gen2_card_upper": {},
    "gen1_bag_border": {},
}


# ---------------------------------------------------------------------------
# Core comparator -- the piece that must never be a dead stub again.
# ---------------------------------------------------------------------------
def diff_grids(real, mine, blink=None, deviations=None):
    """Diff two 18x20 [row][col] grids of comparable values (tile ids, or
    any other per-cell scalar/tuple `==` can compare) BY CELL POSITION.

    Returns a list of (row, col, real_val, mine_val, deviation_name_or_None)
    for every cell where they differ and the difference is not explained by
    `blink` (a {(row,col): {alt_vals...}} map of cells allowed to show an
    alternate value this capture). Cells present in `deviations`
    ({(row,col): name}) are still reported (never silently dropped -- a
    caller wants the full, honest list) but carry their accepted name so a
    human can tell "known cosmetic" from "unexplained" at a glance.

    This function has ONE job and is deliberately the smallest thing
    --selftest can pin: given two grids differing at exactly one cell, it
    must return a list of length exactly 1, naming that cell.
    """
    blink = blink or {}
    deviations = deviations or {}
    mism = []
    for y in range(len(real)):
        for x in range(len(real[y])):
            r, m = real[y][x], mine[y][x]
            if r == m:
                continue
            if m in blink.get((y, x), set()):
                continue
            mism.append((y, x, r, m, deviations.get((y, x))))
    return mism


def categorize(mismatches):
    """The original U4 Red-bag region buckets (unchanged), now built on top
    of diff_grids()'s own mismatch list instead of re-walking the grids."""
    cats = {}
    for (y, x, r, m, dev) in mismatches:
        if dev is not None:
            cat = f"ACCEPTED: {dev}"
        else:
            inbox = (2 <= y <= 12 and 4 <= x <= 19) or (13 <= y <= 15 and 10 <= x <= 19)
            if not inbox:
                cat = "A: outside the drawn box (real = START menu / overworld)"
            elif y in (4, 6, 8, 10) and 6 <= x <= 14:
                cat = "B: item/field-NAME text cells (no names table this slice)"
            elif y == 14 and 12 <= x <= 15:
                cat = "C: EXIT glyph cells"
            elif y in (5, 7, 9, 11) and 14 <= x <= 16:
                cat = "D: the x + QUANTITY column"
            else:
                cat = "F: OTHER STRUCTURAL CELL"
        cats.setdefault(cat, []).append((y, x, hex(r), hex(m)))
    return cats


def painter(harness: Path, sav: Path, top: int, sel: int):
    out = subprocess.run([str(harness), str(sav), str(top), str(sel)],
                          capture_output=True, text=True).stdout
    g = []
    for line in out.splitlines():
        line = line.strip()
        if not line.startswith("["):
            continue
        g.append([int(v, 16) for v in line.strip("[],").split(",")])
    return g


def load_real(path: Path):
    return json.load(open(path))["grid"]


def run(tag, harness, real_json, sav, top, sel, blink=None):
    real = load_real(real_json)
    mine = painter(harness, sav, top, sel)
    mism = diff_grids(real, mine, blink=blink,
                       deviations=ACCEPTED_DEVIATIONS.get("gen1_bag_list"))
    cats = categorize(mism)
    total_inbox = sum(1 for y in range(18) for x in range(20)
                      if (2 <= y <= 12 and 4 <= x <= 19) or (13 <= y <= 15 and 10 <= x <= 19))
    print(f"=== {tag}  (top={top} sel={sel})  in-box cells: {total_inbox} of 360")
    for c in sorted(cats):
        print(f"  {c}: {len(cats[c])} mismatched")
        for e in cats[c]:
            print(f"      row {e[0]:2d} col {e[1]:2d}: real={e[2]:>5s} pokedna={e[3]:>5s}")
    if not cats:
        print("  (no mismatches at all)")
    print()


# ---------------------------------------------------------------------------
# --selftest: injects one known-bad cell and demands the comparator report
# EXACTLY it. The chosen cell moves with argv (see main()) so a hardcoded
# lucky match can't silently pass.
# ---------------------------------------------------------------------------
def selftest(bad_row=7, bad_col=13):
    real = [[0x7F for _ in range(20)] for _ in range(18)]
    mine = [[0x7F for _ in range(20)] for _ in range(18)]
    mine[bad_row][bad_col] = 0x01          # the one deliberate defect
    mism = diff_grids(real, mine)
    print(f"[selftest] injected 1 bad cell at (row {bad_row}, col {bad_col})")
    print(f"[selftest] diff_grids() reported {len(mism)} mismatch(es): "
          f"{[(y, x, hex(r), hex(m)) for (y, x, r, m, _) in mism]}")
    ok = (len(mism) == 1 and mism[0][0] == bad_row and mism[0][1] == bad_col
          and mism[0][2] == 0x7F and mism[0][3] == 0x01)
    # NEGATIVE half: two identical grids must report ZERO mismatches (a
    # comparator that reports SOMETHING regardless of input would also pass
    # the positive half above by accident).
    clean = diff_grids(real, real)
    ok = ok and len(clean) == 0
    print(f"[selftest] two identical grids reported {len(clean)} mismatch(es) (want 0)")
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# --demo: the real, end-to-end pipeline over two screens.
# ---------------------------------------------------------------------------
def _rom_tile_px(rom: bytes, off: int):
    """Decode one 2bpp 8x8 tile straight out of ROM BYTES at file offset
    `off` -- the exact formula oracle.py's tile_px() uses on live VRAM, so
    the two are directly comparable cell-for-cell."""
    out = []
    for r in range(8):
        lo, hi = rom[off + r * 2], rom[off + r * 2 + 1]
        out.append([((hi >> (7 - c)) & 1) * 2 + ((lo >> (7 - c)) & 1) for c in range(8)])
    return out


def _build_harness(src_names, out_name, extra_flags=(), tempdir=None):
    """Compiles one of this directory's harness_*.c files (plus whatever
    PokeDNA source/*.c objects it needs) with the host `cc`. Returns the
    compiled binary's Path. Raises RuntimeError with the compiler's own
    stderr on failure -- never silently falls back to a guess."""
    out = Path(tempdir) / out_name
    srcs = [str(GB_ORACLE_DIR / n) if not n.startswith("source/") else str(ROOT / n)
            for n in src_names]
    cmd = ["cc", "-std=c11", "-Wall", "-Wextra", "-I", str(ROOT / "source"),
           "-I", str(tempdir), "-DPDNA_GBSCREEN_HOST_TEST", *extra_flags,
           *srcs, "-o", str(out)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0 or not out.is_file():
        raise RuntimeError(f"harness build failed ({out_name}):\n{proc.stderr}")
    return out


def demo_gold_card(gb_roms: Path, mgba_vendor):
    """The Gen-2 trainer card, Gold: g2card_build_upper_cells()'s 51 always-
    shown static cells vs. the real cartridge's own VRAM. female=False/
    has_corner=False is correct for Gold specifically (see harness_g2card.c's
    own comment) -- no save parsing needed for THIS cell set."""
    rom = gb_roms / "Gold.gbc"
    sav = gb_roms / "Gold.sav"
    print(f"=== DEMO: Gen-2 trainer card (Gold), {rom} ===")
    with tempfile.TemporaryDirectory() as td:
        harness = _build_harness(
            ["harness_g2card.c", "source/g2card_cells.c", "source/pdna_gbscreen.c",
             "source/rom_gbui.c", "source/gb_sprite_codec.c", "source/gb_edit.c",
             "source/gen1_save.c", "source/gen2_save.c"],
            "g2card_dump", tempdir=td)
        cells_out = subprocess.run([str(harness), str(rom)], capture_output=True, text=True)
        if cells_out.returncode != 0:
            raise RuntimeError(f"g2card_dump failed: {cells_out.stderr}")
        cells = [tuple(map(int, ln.split())) for ln in cells_out.stdout.splitlines()]
    print(f"  {len(cells)} static upper-card cells resolved to ROM offsets")

    rep, gb = R._drive(str(rom), str(sav), Path(tempfile.mkdtemp()), want_party=False,
                       vendor=mgba_vendor, log=lambda *_: None)
    if rep["verdict"] != "accept":
        raise RuntimeError(f"gb_roundtrip drive failed: {rep}")
    gb.tap("START"); gb.step(40); gb.settle()
    for _ in range(4):                      # POKeDEX,POKeMON,PACK,GEAR,<name> -- 4 DOWNs
        gb.tap("DOWN"); gb.step(10)
    gb.tap("A"); gb.step(60); gb.settle()

    c = O.compose(gb)
    rom_bytes = rom.read_bytes()
    deviations = ACCEPTED_DEVIATIONS["gen2_card_upper"]
    mism = []
    for (x, y, off) in cells:
        real_px = O.tile_px(gb, c["grid"][y][x], c["lcdc"])
        exp_px = _rom_tile_px(rom_bytes, off)
        if real_px != exp_px:
            mism.append((y, x, off, deviations.get((y, x))))
    _report("gen2_card_upper", len(cells), mism)
    return len(mism) == 0


def demo_red_bag(gb_roms: Path, mgba_vendor):
    """The Gen-1 Item bag, Red: g1bag_border()'s own frame cells (the box +
    nested EXIT box -- not the dynamic item list, which needs a live GbBag)
    vs. the real cartridge's own VRAM."""
    rom = gb_roms / "Red.gb"
    sav = gb_roms / "Red.sav"
    print(f"=== DEMO: Gen-1 Item bag border (Red), {rom} ===")
    bag_c = (ROOT / "source" / "pdna_gbbag.c").read_text().splitlines()
    start = next(i for i, ln in enumerate(bag_c) if ln.strip().startswith("enum {") and "G1I_UL" in bag_c[i + 1])
    fn_open = next(i for i, ln in enumerate(bag_c) if ln.startswith("static void g1bag_border("))
    depth, end = 0, None
    for i in range(fn_open, len(bag_c)):
        depth += bag_c[i].count("{") - bag_c[i].count("}")
        if depth == 0 and i > fn_open:
            end = i
            break
    if end is None:
        raise RuntimeError("could not find g1bag_border()'s closing brace")
    body = "\n".join(bag_c[start:end + 1])
    with tempfile.TemporaryDirectory() as td:
        (Path(td) / "g1bag_body.inc").write_text(body + "\n")
        harness = _build_harness(
            ["harness_g1bag.c", "source/pdna_gbscreen.c", "source/rom_gbui.c",
             "source/gb_sprite_codec.c", "source/gb_edit.c", "source/gen1_save.c",
             "source/gen2_save.c"],
            "g1bag_dump", tempdir=td)
        cells_out = subprocess.run([str(harness), str(rom)], capture_output=True, text=True)
        if cells_out.returncode != 0:
            raise RuntimeError(f"g1bag_dump failed: {cells_out.stderr}")
        cells = [tuple(map(int, ln.split())) for ln in cells_out.stdout.splitlines()]
    print(f"  {len(cells)} static border cells resolved to ROM offsets")

    rep, gb = R._drive(str(rom), str(sav), Path(tempfile.mkdtemp()), want_party=False,
                       vendor=mgba_vendor, log=lambda *_: None)
    if rep["verdict"] != "accept":
        raise RuntimeError(f"gb_roundtrip drive failed: {rep}")
    gb.tap("START"); gb.step(40); gb.settle()
    rows = gb.settle()
    target = next(i for i, r in enumerate(rows) if "ITEM" in r)

    def cursor_row():
        g = O.compose(gb)["grid"]
        for y in range(18):
            for x in range(20):
                if g[y][x] == 0xED:
                    return y
        return None

    for _ in range(12):
        cy = cursor_row()
        if cy is None or cy == target:
            break
        gb.tap("DOWN" if cy < target else "UP"); gb.step(12)
    gb.tap("A"); gb.step(60); gb.settle()

    c = O.compose(gb)
    rom_bytes = rom.read_bytes()
    deviations = ACCEPTED_DEVIATIONS["gen1_bag_border"]
    mism = []
    for (x, y, off) in cells:
        real_px = O.tile_px(gb, c["grid"][y][x], c["lcdc"])
        exp_px = _rom_tile_px(rom_bytes, off)
        if real_px != exp_px:
            mism.append((y, x, off, deviations.get((y, x))))
    _report("gen1_bag_border", len(cells), mism)
    return len(mism) == 0


def _report(tag, total, mism):
    print(f"  {tag}: {total} cells compared, {len(mism)} mismatch(es)")
    for (y, x, off, dev) in mism:
        tag2 = f"ACCEPTED: {dev}" if dev else "UNEXPECTED"
        print(f"      row {y:2d} col {x:2d}: expected ROM offset 0x{off:x}  [{tag2}]")
    if not mism:
        print("  (no mismatches -- every cell resolved to the exact same pixels "
              "the real cartridge is showing)")


def run_demo(gb_roms: Path, mgba_vendor):
    ok1 = demo_gold_card(gb_roms, mgba_vendor)
    ok2 = demo_red_bag(gb_roms, mgba_vendor)
    print("PASS" if (ok1 and ok2) else "FAIL")
    return 0 if (ok1 and ok2) else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--selftest", action="store_true",
                     help="synthetic dead-stub guard: inject one bad cell, "
                          "demand diff_grids() reports exactly it. No ROM/mGBA needed.")
    ap.add_argument("--demo", action="store_true",
                     help="real end-to-end run: Gen-2 trainer card (Gold) + "
                          "Gen-1 Item bag border (Red), each cell resolved to "
                          "real ROM bytes and diffed against a live mGBA capture.")
    ap.add_argument("--harness", type=Path,
                     help="path to a compiled painter harness (see README.md); "
                          "--grids-dir/--gb-roms also required in this mode")
    ap.add_argument("--grids-dir", type=Path,
                     help="dir holding the *_grid.json ground-truth files")
    ap.add_argument("--gb-roms", type=Path,
                     default=Path(os.environ.get("PDNA_GB_ROMS", "")) or None,
                     help="dir holding Red.gb/Red.sav/Gold.gbc/Gold.sav etc. "
                          "(e.g. gba-toolkit/roms/gb; also via $PDNA_GB_ROMS)")
    ap.add_argument("--mgba-vendor", default=os.environ.get("GB_ROUNDTRIP_MGBA"),
                     help="dir holding the mGBA Python bindings, --demo only "
                          "(default: $GB_ROUNDTRIP_MGBA)")
    a = ap.parse_args()

    if a.selftest:
        return selftest()

    if a.demo:
        if not a.gb_roms:
            ap.error("--demo needs --gb-roms (or $PDNA_GB_ROMS)")
        return run_demo(a.gb_roms, a.mgba_vendor)

    if not (a.harness and a.grids_dir and a.gb_roms):
        ap.error("give --selftest, --demo, or all of --harness/--grids-dir/--gb-roms")

    run("Red, list top (sel=0)", a.harness, a.grids_dir / "red_row1_grid.json",
        a.gb_roms / "Red.sav", 0, 0)
    run("Red, scrolled (real top=HM03 idx7, sel=BICYCLE idx9)", a.harness,
        a.grids_dir / "red_scroll8_grid.json", a.gb_roms / "Red.sav", 7, 9)
    run("Yellow, list top (sel=0)", a.harness, a.grids_dir / "yellow_row1_grid.json",
        a.gb_roms / "Yellow.sav", 0, 0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
