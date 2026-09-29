#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""dgb_shots.py — re-run the GB shot flows on pokedna-delta-gb.gba (BACKLOG #62, #68a).

WHY A SEPARATE SCRIPT (does NOT edit tools/gb_shots.py)
---------------------------------------------------------
tools/gb_shots.py drives PokeDNA's Game Boy fork against a build fused with exactly
ONE GB save (tools/fuse_sav.py --gb) — boot lands straight on S1's box grid via
view_save()'s own standalone GB-size fork (memset g_vinfo, pdna_gen12_show_image over
a resident buffer): a full read/write-capable mount (VIEW/EDIT combined, MOVE TO BOX,
RELEASE, CREATE on an empty cell). pokedna-delta-gb.gba (tools/fuse_gb.py, BACKLOG #62)
fuses Emerald.sav (the Gen-3 flash seed) TOGETHER WITH the Red/Gold/Crystal directory.

#68a: view_save() now offers a BOOT PICKER whenever a Gen-3 save is READY -- the flash
chip parses, or (the normal case on a fresh emulator boot, where the flash is blank) the
single-slot fused .sav fallback supplies it -- AND a GB corpus is fused in (gb_delta_boot_pick(), source/pdna_main.c) — row 0 is
the loaded Gen-3 save, rows 1..n mirror the fused GB saves one for one. Picking the
Gen-3 row continues into the normal Emerald box screen, where the nav menu's "GB
import" row (NV_GB) is RETIRED by BACKLOG #239/#277 — it now shows the BANK ONLY
refusal (it used to offer the READ-ONLY nested mount via pdna_gen12_show_fused() —
VIEW / LEGALITY / COPY / CANCEL — and the chains here now pin the refusal instead). Picking a GB row instead reuses the SAME full read/write STANDALONE
mount tools/gb_shots.py's own run_gold()/run_red() were calibrated against — VIEW/EDIT,
LEGALITY, MOVE TO BOX, COPY, RELEASE, and a REAL, WORKING CREATE (this build fuses
actual Red.gb/Gold.gbc/Crystal.gbc ROMs alongside the saves, unlike gb_shots.py's
SD-based build, which has no fused ROM and always refuses CREATE). Backing all the way
out of a GB mount (B) reloads the Gen-3 save from flash and re-shows the SAME boot
picker — retired Makefile target `delta-gb-only` used to be the only way to reach this
standalone mount (a second, blank-flash image); the picker makes it reachable from the
one combined image instead, so this script now drives everything off `--image` alone.

#62 review D9's COLD-FETCH COST: the first time ANY generation's sprite/icon art is
fetched this session, gb_art_source.c's PDNA_DELTA half does a from-scratch whole-ROM
scan (no loc-cache entry yet) — measured ~250-260 s of EMULATED GBA time (matches the
D1 review's own "~240 s per cell" estimate) for a 1 MB Gen-1 ROM. Every FOLLOWING fetch
of that same generation reuses the loc cache D1 built and is fast (measured ~7 s of
emulated time, a ~38x speedup). GB_ART_COLD_SETTLE below rides out the first, slow
fetch per generation; ordinary BIG_SETTLE covers everything after it. Getting this
wrong does not fail loudly — it just captures whatever partial-repaint frame mGBA
happens to be on when the settle window ends (a checkerboard placeholder, not real
art), so this constant is deliberately generous, not tuned to the bare minimum.

Usage:
    /usr/local/bin/python3 tools/dgb_shots.py --image pokedna-delta-gb.gba \
        --out docs/shots/gb

BACKLOG #179 lane s179-a4, phase A5 SCOPE CUT (approved by the orchestrator, not a
completion): A5 asked to convert THREE conversion chains to --vsd -- --s150-8,
--s150-8-bridge, --r1-xfer. Only run_s150_8_bridge() (--s150-8-bridge) was converted
(see its own docstring below). The other two were NOT converted, each blocked by a
pre-existing problem this lane diagnosed but did not fix, now filed as its own
backlog item so a future reader does not mistake this scope cut for A5 being done:
  BACKLOG #236 -- run_s150_8_gen3_arm()'s (--s150-8) own enter_bank() helper is
    missing the dismiss tap its sibling run_s150_8_bridge()'s boot_to_grid() already
    has for the "GAME BOY SAVE / Edits are in-session only in the emulator build."
    msg_wait (source/pdna_gen12.c:4422, gb_persist()'s unconditional #ifdef
    PDNA_DELTA refusal -- NOT the VSD seam). Fixing it moves non-vsd frames, which
    the A4 parity sweep this same lane wrote forbids touching mid-lane.
  BACKLOG #237 -- run_r1_xfer()'s (--r1-xfer) own docstring needs a bespoke
    Charizard-L20 clip-seeded fused image (an ordinary Gen-3 .sav for the boot
    picker's row 0 PLUS an 80-byte fuse_sav.py --clip seed PLUS one GB ROM+save)
    this lane could not reconstruct in its remaining budget.
"""
from __future__ import annotations

import argparse
import functools
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots  # noqa: E402 -- Session, load_mgba, KEY, HOLD/SETTLE/BIG_SETTLE
import gb_claims  # noqa: E402 -- BACKLOG #184: --selftest-captions's offline claim re-check
import gen_gbfields  # noqa: E402 -- BACKLOG #129: GROUPS_GEN1's own header order,
                     # imported directly rather than a second hand-copied list
import fuse_gb   # noqa: E402 -- BACKLOG #98 D3: reads the fused image's OWN directory
                 # (locate_record_permissive/read_record/parse_directory, the same
                 # code path `fuse_gb.py --check` uses) instead of assuming a fixed
                 # fuse order.


@functools.lru_cache(maxsize=None)
def gb_save_pick_index(image: Path) -> dict[str, int]:
    """BACKLOG #98 D3 (review-sonnet ab81b56): the boot picker's GB rows mirror
    fused_gb_save()'s own enumeration order -- SAV-type directory entries, in
    directory order, offset by +1 (row 0 is the Gen-3 save) -- so the picker row
    for a given save NAME depends on the fuse ORDER of THIS image, not some fixed
    Red/Gold/Crystal assumption. The old PICK_INDEX = {"red": 0, "gold": 1,
    "crystal": 2} hardcoded the Makefile's delta-gb recipe order and silently
    drove the wrong row against any image fused in a different order (e.g. a
    Crystal-first multi-ROM harness image built to reproduce BACKLOG #98 D1).

    Reads `image`'s own fused directory the same way `fuse_gb.py --check` does
    (locate_record_permissive -> read_record -> parse_directory) and maps each
    SAV entry's filename stem, lowercased, to its enumeration index -- e.g.
    {"gold": 0, "crystal": 1} for a Crystal-first two-save image, independent of
    what generation/order the caller expects. Memoized per image path: this
    script calls it once per shot, and the image never changes mid-run.
    """
    blob = image.read_bytes()
    rec_off = fuse_gb.locate_record_permissive(blob, str(image))
    off, size = fuse_gb.read_record(blob, rec_off)
    if not size:
        return {}
    entries = fuse_gb.parse_directory(blob, off, size)
    out: dict[str, int] = {}
    idx = 0
    for e in entries:
        if e["type"] != fuse_gb.TYPE_SAV:
            continue
        stem = Path(e["name"]).stem.lower()
        out[stem] = idx
        idx += 1
    return out

# BACKLOG #118 (coordinator addition, a74 audit): the OLD `NV_GB_DOWN_FROM_COL1_TOP =
# 6` hand-copied literal drifted stale the moment NV_MAP was inserted ahead of NV_GB
# in source/pdna_layout.h's PDNA_NAV_ITEMS -- the harness silently landed on the Map
# screen's own "PICK YOUR ROM (.gba)" prompt instead of the GB-import picker, and
# BOTH of Session.shot()'s own guards (flat-colour, identical-to-previous) let that
# wrong-screen frame through uncaught. Fixed two ways: (1) the offset is now derived
# from source/pdna_layout.h's own PDNA_NAV_ITEMS list every run, the same "read the
# generator's own order, never hand-copy it" rule tools/gen_gbfields.py already
# follows for the GB field tables; (2) nav_to_gb_refusal() below (BACKLOG #277: was nav_to_gb_import(), which asserted the
# retired picker) claim-checks the landed BANK ONLY refusal; the original guard was the SAME "PICK A SAVE" crop-signature boot_to_gb_session() uses --
# gb_delta_pick_save() (pdna_main.c:8559) draws that exact title for BOTH the
# top-level boot fork AND this nested NV_GB import (pdna_main.c:9151's own case
# NV_GB calls the identical function), so no second reference PNG is needed: a
# byte-identical screen does not need a byte-identical-but-differently-named ref.
_NAV_LAYOUT_H = ROOT / "source" / "pdna_layout.h"


@functools.lru_cache(maxsize=None)
def _nav_item_order() -> list[str]:
    """PDNA_NAV_ITEMS(X)'s own item order, parsed from source/pdna_layout.h --
    never hand-copied. Returns the NV_* identifiers in on-screen index order
    (index 0 == NV_PARTY, the menu's own top-left row). NOTE (h118 review): the screen refs are
    save-independent by design, so a miscount that lands on an ADJACENT VALID row of the same kind
    (e.g. Crystal's info page instead of Gold's) is NOT caught here -- only a wrong-screen landing is."""
    text = _NAV_LAYOUT_H.read_text(encoding="utf-8")
    m = re.search(r"#define PDNA_NAV_ITEMS\(X\)(.*?)\n\n", text, re.S)
    if not m:
        raise RuntimeError(f"{_NAV_LAYOUT_H}: PDNA_NAV_ITEMS(X) macro body not found "
                            f"-- pdna_layout.h's own shape changed, fix this parser")
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)   # h118 review: a commented-out X(NV_…) must not count
    items = re.findall(r"X\((NV_\w+),", body)
    if not items:
        raise RuntimeError(f"{_NAV_LAYOUT_H}: parsed ZERO X(NV_*, ...) entries out of "
                            f"PDNA_NAV_ITEMS -- the regex above no longer matches")
    return items


def nav_down_from_col_top(name: str) -> int:
    """DOWN count from the top of `name`'s own column (reached by ONE RIGHT press
    from a fresh menu, source/pdna_main.c nav_menu(): KEY_RIGHT does `sel += rows`
    when `sel + rows < NV_COUNT`, i.e. it jumps to index `rows` -- the first row of
    column 1) down to `name`'s own row. PDNA_NAV_ROWS's own formula, replicated
    here from source/pdna_layout.h (`(PDNA_NAV_COUNT + 1) / 2`): with today's 20
    items that is 10 rows per column, so index 17 (NV_GB) is row 7 of column 1 --
    NOT the stale hand-copied 6 this file used before BACKLOG #118's fix."""
    items = _nav_item_order()
    idx = items.index(name)
    rows = (len(items) + 1) // 2
    if idx < rows:
        raise ValueError(f"{name}: index {idx} is in column 0, not column 1 -- "
                          f"nav_down_from_col_top() only answers for a RIGHT-then-"
                          f"DOWN column-1 approach; DOWN x{idx} alone reaches it "
                          f"from a fresh menu (nav_menu's DOWN wraps the WHOLE "
                          f"list linearly, not per column)")
    return idx - rows


NV_GB_REFUSAL_CLAIM = ["BANK ONLY", "Open the GB save on its own,", "send it to the Bank, come back."]


def nav_to_gb_refusal(s: gb_shots.Session, name: str, caption: str) -> None:
    """BACKLOG #277 (was nav_to_gb_import(), which asserted the retired nested
    "PICK A SAVE" mount): START (box grid -> nav menu) -> RIGHT -> DOWN x(NV_GB's own
    row, derived from source every run) -> A -> shoots `name` with the "BANK ONLY"
    refusal claim-checked, then A dismisses the dialog (msg_wait takes A only). BACKLOG
    #239 closed NV_GB on a live Gen-3 save (nav_avail.c nv_gb_blocked(),
    xfer_direct_allowed() is a constant false), so a chain that reaches this from an
    Emerald box grid MUST land on the refusal -- if the mount is ever reopened the
    claim fails loudly instead of the chain silently shooting whatever opened."""
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box screen -> nav menu
    s.tap("RIGHT")                                            # column 0 (Party) -> column 1 (Blocks)
    s.press_n("DOWN", nav_down_from_col_top("NV_GB"))          # Blocks -> ... -> GB import
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # NV_GB -> refuse, not a mount
    s.shot(name, caption, claim=NV_GB_REFUSAL_CLAIM)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # msg_wait dismisses on A

# BACKLOG #185 (F1 gen-aware jobs + F2 inline prefilter gates) re-measured the
# SPRITE/PORTRAIT half of this cold scan directly in THIS emulator, same harness as
# run_cold_start_compare()/_measure_box_grid_cold_start() below (a --no-loc fused
# image, so no baked .loc seed short-circuits it): Red.sav's cold portrait fetch
# dropped from 14,935 frames (250.05 s, the OLD 6-job unrestricted scan -- matches
# this comment's prior "~258 s" almost exactly) to 1,280 frames (21.43 s) after F1+F2;
# Crystal.sav's dropped from 17,785 frames (297.77 s) to 2,915 frames (48.80 s). This
# lane did NOT touch rom_gbicon.c's menu-icon scan (a separate locator, separate
# job set) -- so the OLD combined "Gen 2 pays TWO independent cold scans ... ~500 s"
# figure's ICON half is assumed UNCHANGED: 500 - 297.77 (the old portrait-only
# share measured here) ~= 202 s of icon-scan cost, not independently re-measured
# this lane. New Gen-2 worst case estimate = 48.80 (new portrait) + 202 (unchanged
# icon, carried over) ~= 251 s = 14,991 frames; +50% margin (this file's own
# convention) rounds to 22,500 frames = ~377 s, comfortably over that estimate
# while still ~30% of the old 32,000/533 s provision -- the actual per-screen
# savings this backlog item was for.
GB_ART_COLD_SETTLE = 22500

# BACKLOG #196 (fix pass, 2026-09-22): a GB-served dex PAGE PAINT is 21 real per-cell
# ROM fetches (pdna_origin_art_portrait_by_dex() for Gen 1, pdna_origin_art_icon() for
# Gen 2) -- before this backlog Red's dex grid drew instant name-chip text (no ROM I/O
# at all), so BIG_SETTLE (40 frames) was always enough for Red; it is NOT enough now.
# Measured (frame-diff probe, after the whole-ROM cold scan was already paid via
# GB_ART_COLD_SETTLE earlier in the SAME navigation): Red's grid area is still 3,814
# nonzero px at +60 frames after the L-press and 0 at +90; 180 is that measured
# stabilization point (~150 frames) plus ~20% margin, not a guess. Crystal's OWN page
# was already stable well inside BIG_SETTLE when probed the same way -- applied here
# too anyway (a fix-pass review finding: keeping BOTH games on the SAME settle,
# gated only on "a ROM is actually present" (`not fallback`), is simpler and more
# robust than a per-generation special case that assumes Gen 2 will always stay
# cheap -- costs a little extra emulated time on Crystal, changes nothing it shows).
GB196_GB_PAGE_SETTLE = 180

# BACKLOG #118 (orchestrator ruling 2026-09-12, after an h118 STOP on the brief's
# original oracle.py-based design -- oracle.py's compose() reads Game Boy PPU
# registers/VRAM off a bare `GbDriver` GB/GBC core (tools/gb_roundtrip.py:356);
# this script's Session wraps a GBA core (source/ui.c:8 `DCNT_MODE3` -- a bitmap
# framebuffer, no tilemap at all), so there is no tilemap cell for that tool to
# read here). The replacement: an exact-match fixed-crop pixel signature against
# a committed reference PNG, on a screen band chosen to hold no save-specific
# text (no trainer name/ID/box name) so ONE reference serves every game/save.
GB_ORACLE_REFS = ROOT / "tools" / "gb_oracle" / "refs"


def _crop_for(name: str) -> tuple[int, int, int, int]:
    """The fixed pixel band each named screen is identified by -- see
    boot_to_gb_session()'s own docstring for the step-0 finding these support."""
    return {
        # pdna_main.c gb_delta_pick_save()/gb_delta_boot_pick() both open with
        # `ui_text(4, 3, UI_TITLE, "PICK A SAVE"); ui_hline(0, 13, UI_SCR_W, ...);`
        # -- the row list (save names) starts at y=18, well below this crop.
        "pick_a_save": (0, 0, 240, 14),
        # SAME title-band shape, pdna_gen12.c:815-816's "GAME BOY SAVE" header --
        # the player name/ID line is ui_ptext_fit()'d at y=18, below this crop.
        "gb_info_page": (0, 0, 240, 14),
        # source/pdna_box.c render_full()'s 3-tab row (y=0..12): "PKMN DATA" /
        # "(BANK)" (src->is_bank -- every GB source sets this true) / "SAVE" --
        # fixed literals for any GB source, regardless of which box/save is
        # loaded. The box name + occupancy count draw_box_banner() paints is at
        # y=13+, below this crop -- that IS save-specific and deliberately excluded.
        "gb_box_grid": (0, 0, 240, 12),
        # BACKLOG #129: pdna_gbflags.c nf_draw_row()'s own selected-header row --
        # "+ Story" (fold_glyph + name), the SEL panel spanning the full row
        # (source: `ui_panel(2, y - 1, 236, 9, ...)`, y determined empirically for
        # THIS deterministic tap sequence: fresh FLAGS-tab entry, header 0
        # unfolded, then SELECT xN to "Story" -- the row text itself comes from
        # GROUPS_GEN1's own header name (tools/gen_gbfields.py), not per-save
        # data, so this crop is game/save-independent for Red/Yellow the same
        # way the other refs are.
        "gbflags_story_header": (0, 138, 240, 149),
        # BACKLOG #135: pdna_gbtrainer_gen1_card()'s title row (1:1 card view) --
        # a fixed header band with no save-specific content, used to validate
        # landing on the trainer card screen before shooting (run_u2c_trainer shots 10-11).
        "gen1_trainer_card": (0, 0, 240, 14),
    }[name]


def _current_crop(s: gb_shots.Session, name: str):
    return s.screen.to_pil().convert("RGB").crop(_crop_for(name))


def screen_is(s: gb_shots.Session, name: str) -> bool:
    """Non-raising check: does s's CURRENT frame match the fixed-crop reference
    tools/gb_oracle/refs/<name>.png exactly (pixel-for-pixel)? Used by
    boot_to_gb_session() to detect whether the boot picker is showing at all --
    BACKLOG #118 step 0's finding is that it isn't, on a single-fused-GB-save
    image with no Emerald.sav (gb_delta_pick_save's own `if (n == 1) return 0;`,
    pdna_main.c:8567)."""
    from PIL import Image, ImageChops
    ref = Image.open(GB_ORACLE_REFS / f"{name}.png").convert("RGB")
    cur = _current_crop(s, name)
    return ImageChops.difference(ref, cur).getbbox() is None


def assert_screen(s: gb_shots.Session, name: str) -> None:
    """Raising counterpart of screen_is(): on a miss, dumps the full current
    frame AND the mismatched crop next to the run's other shots before raising,
    so a wrong landing is a screenshot away, never a guess."""
    if screen_is(s, name):
        return
    full_path = s.out_dir / f"{s.prefix}{name}_MISMATCH_full.png"
    crop_path = s.out_dir / f"{s.prefix}{name}_MISMATCH_crop.png"
    s.screen.to_pil().convert("RGB").save(full_path)
    _current_crop(s, name).save(crop_path)
    raise AssertionError(
        f"assert_screen: expected screen '{name}' but the current frame's "
        f"{_crop_for(name)} crop does not match tools/gb_oracle/refs/{name}.png "
        f"-- see {full_path} and {crop_path}")


def row_index(rom: Path, which: str | None) -> int:
    """The DOWN count from the picker's own row 0 to `which`'s row.
    gb_delta_boot_pick's row order (pdna_main.c:8612-8622, read from source):
    row 0 is ALWAYS the loaded Gen-3 save (g3_label); rows 1..n mirror
    fused_gb_save()'s own enumeration one for one, i.e. THIS image's fuse
    order -- read live via gb_save_pick_index() (BACKLOG #98 D3), never a
    hardcoded Red/Gold/Crystal guess. which=None means "stay on row 0" (the
    Gen-3 save itself, e.g. to reach the flight image's Emerald session)."""
    if which is None:
        return 0
    return gb_save_pick_index(rom)[which] + 1


def select_jumps_to(groups: list[tuple[str, list]], from_title: str, to_title: str) -> int:
    """BACKLOG #129: SELECT count to jump from `from_title`'s own header to
    `to_title`'s, on pdna_gbflags.c's FLAGS tab -- SELECT jumps by header INDEX
    regardless of fold state (nf_draw_row's own header rows), so the count is
    just the difference of the two titles' positions in the generator's OWN
    group list (`groups`, e.g. tools/gen_gbfields.py's GROUPS_GEN1/GROUPS_GEN2)
    -- never a second, hand-copied header list that can drift out of sync with
    the generator's real one (the exact class of bug BACKLOG #118's coordinator
    addition found for NV_GB's own DOWN count)."""
    titles = [title for title, _members in groups]
    return titles.index(to_title) - titles.index(from_title)


def boot_to_gb_session(s: gb_shots.Session, rom: Path, which: str | None = None) -> None:
    """Shared boot prefix for EVERY run_* that opens a GB session -- covers both
    image shapes BACKLOG #118 step 0 found in mGBA on 2026-09-12:

      - a single-fused-GB-save image (tools/fuse_gb.py fed exactly one ROM+save
        pair, no Emerald.sav) skips the "PICK A SAVE" picker ENTIRELY --
        gb_delta_pick_save()'s own `if (n == 1) return 0;` (pdna_main.c:8567) --
        landing straight on that save's own box grid (#279: direct entry, no info page)
        the instant `s.run(700)` finishes (verified: Red-only and Gold-only images
        both enter the box grid with zero picker frames).
      - any image with a Gen-3 save ready (flash_ok or the fused-.sav fallback)
        AND a fused GB corpus -- e.g. `make delta-gb`'s own Emerald.sav +
        Red/Gold/Crystal recipe, the "flight" shape -- ALWAYS shows the picker:
        gb_delta_boot_pick() (pdna_main.c:8612) has NO n==1 shortcut anywhere in
        its body (unlike gb_delta_pick_save). This is BACKLOG #118's own claim,
        confirmed true for this image shape (verified: a Red+Gold+Crystal+
        Emerald.sav image shows "PICK A SAVE" with rows "Gen 3 save (Emerald/
        FR/LG)" (row 0, default-selected), "Red.sav", "Gold.sav", "Crystal.sav").
      Lane tiny3's "single-ROM image, no picker" result and BACKLOG #118's
      "fused build ALWAYS shows PICK A SAVE" claim are both correct -- about
      different image shapes, not a contradiction.

    `which` (a fused GB save's filename stem, e.g. "red"/"gold"/"crystal") picks
    a row via row_index() when the picker IS showing; ignored otherwise (a
    single-ROM image has nothing to pick). Either way this ends on the box
    grid, ASSERTING each landing against a fixed-crop pixel reference
    (screen_is()/assert_screen(), tools/gb_oracle/refs/*.png) rather than
    trusting the tap count blindly -- a DOWN count that lands on a different screen raises here, with a
    screenshot, instead of silently shooting the wrong save's screens."""
    s.run(700)
    if screen_is(s, "pick_a_save"):
        n = row_index(rom, which)
        if n:
            s.press_n("DOWN", n, settle=gb_shots.SETTLE)   # Gen-3 row -> `which`'s row
        s.tap("A", settle=60)                               # pick row -> box grid (#279: no info page)
    s.run(GB_ART_COLD_SETTLE)
    assert_screen(s, "gb_box_grid")


def run_gbscreen_shell(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """U2a (docs/GB-GAME-SCREENS-DESIGN.md sec 3.3): the shared GB-screen shell's own
    standalone demo (source/pdna_gbscreen.c gbscr_run_demo()) -- the located font's
    128-glyph sheet inside a text-box border, reachable via a hidden SELECT key on
    the Settings list (no card exists yet; that is U2b). This build boots into
    #68a's own boot picker first (see run_nav_gb()'s own module-docstring note --
    row 0 is the Emerald save, rows 1..n the fused GB corpus); A on the default
    Emerald selection continues into the normal Gen-3 box screen, from which
    navigation up to the Settings list is IDENTICAL to run_e4_settings()'s own
    (tools/gb_shots.py): START -> nav menu -> RIGHT (col 1) -> DOWN x7 -> A.

    gbscr_open() under PDNA_DELTA does a full, uncached rom_gbui scan every time
    (this slice's EWRAM budget forbids a new EWRAM loc-cache the way #62 D1 gave
    gb_art_source.c's own delta half) -- ridden out with the same GB_ART_COLD_SETTLE
    window dgb_shots.py's own run_nav_gb()/run_standalone() already use for the
    analogous rom_gbsprite cold scan. Only ONE scan per demo ENTRY: gbscr_run_demo()
    opens once and loops on SELECT/A/B without re-opening."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "u2a_")
    print("== U2a: the GB-screen shell demo (Settings > SELECT) ==")

    # Same extra settle run_nav_gb() uses for this SAME image (Emerald.sav + the
    # whole GB directory boots slower than a plain single-fused-save image --
    # Session.__init__'s own 180-frame settle is not enough; without this, START
    # fires mid-load and lands somewhere other than the box screen's own menu).
    s.run(700)
    s.tap("A", settle=gb_shots.BIG_SETTLE)        # #68a boot picker, Emerald row (default) -> box
    s.tap("START", settle=gb_shots.BIG_SETTLE)   # box screen -> nav menu
    s.tap("RIGHT")                                 # column 0 (Party) -> column 1 (Blocks)
    # BACKLOG #118 (coordinator addition, a74 audit): this was hand-copied as
    # DOWN x7 ("index 17") -- but index 17 is NV_GB, not NV_SETTINGS (index 18,
    # row 8 of column 1) -- the SAME stale-offset bug class as NV_GB_DOWN_FROM_
    # COL1_TOP, caught by the same audit and fixed the same way: derived from
    # source every run instead of hand-copied.
    s.press_n("DOWN", nav_down_from_col_top("NV_SETTINGS"))   # Blocks -> ... -> Settings
    s.tap("A", settle=gb_shots.BIG_SETTLE)         # nav menu -> the Settings list

    s.tap("SEL", settle=GB_ART_COLD_SETTLE)        # hidden key -> gbscr_run_demo(PDNA_GEN1)
    s.shot("01_1to1", "U2a: the GB-screen shell demo -- Red.gb's own located font, "
                       "all 128 glyphs (charmap 0x80..0xFF), inside a border of "
                       "RomGbUi.textbox tile 0, 1:1 centred at (40,8) with the "
                       "legend in the side bars (default mode, gb_scale_mode=0). "
                       "D9: tile 0 of pokered's TextBoxGraphics "
                       "(gfx/font/font_extra.2bpp) renders as a bold 'A', not a "
                       "dialogue-box frame -- the actual frame pieces are tiles "
                       "~24-31, and painting a proper frame is U2b's job, not "
                       "this demo's (it only proves the shell fetches and blits "
                       "a non-font ROM block).")

    s.tap("SEL", settle=60)                        # toggle -> stretched
    s.shot("02_stretched", "U2a: SELECT toggles to stretched (240x160, "
                            "blit_stretched's own y_dst0/y_dst_count tables + "
                            "x's period-2 shift/mask formula -- x duplicates "
                            "every 2nd source column, y every 9th source row) "
                            "-- same font, same tile-0 'A' border (D9, see "
                            "01_1to1's caption), filling the whole screen; the "
                            "bottom scrim now shows the shell's own keys "
                            "'A OK  B BACK  SEL SIZE' plus this screen's 'EXIT' "
                            "beside them (D3 fix -- stretched mode used to "
                            "replace the shell's keys with the screen's own)")

    s.tap("SEL", settle=60)                        # toggle back -> 1:1
    s.shot("03_back_1to1", "U2a: SELECT again returns to 1:1 -- a live toggle, not "
                            "a one-way switch")

    s.tap("B", settle=gb_shots.BIG_SETTLE)          # leave the demo -> Settings list
    s.shot("04_settings_after", "U2a: back at the Settings list -- the demo's "
                                 "content is fully gone (pdna_settings() forces a "
                                 "full repaint on return), proving the shell does "
                                 "not leave stray pixels behind on exit")
    return s


def run_nav_gb(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #277 (was BACKLOG #62's nested NV_GB import shots -- info page, real-art
    box grid, occupied-cell menu, VIEW portrait, one set per `which`): that mount was
    CLOSED on purpose by BACKLOG #239 (A on GB import from a live Gen-3 save shows the
    "BANK ONLY" dialog and mounts nothing), so those frames are RETIRED. The same real-
    art GB screens are still proven through the boot picker's own direct GB rows
    (run_standalone(), run_b132_portrait()'s mount 1, run_b64_import()'s direct leg).
    This run now pins the refusal itself: boot picker Emerald row -> box grid -> START >
    GB import must land on "BANK ONLY", claim-checked, so a reopened mount fails loudly.
    `rom` is `make delta-gb`'s combined image (Emerald.sav + the fused GB corpus)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "dgb_nvgb_")
    print("== delta-gb: NV_GB on a live Gen-3 save -- the BANK ONLY refusal (BACKLOG #277) ==")
    s.run(700)
    s.tap("A", settle=gb_shots.BIG_SETTLE)              # #68a boot picker, Emerald row (default) -> box
    s.shot("00_box", "#277: Emerald box grid, freshly booted (boot picker -> Gen-3 row)")
    nav_to_gb_refusal(s, "01_refused",
                      "#277: A on START > GB import shows the \"BANK ONLY\" dialog "
                      "(BACKLOG #239) -- the nested import mount this run used to "
                      "screenshot (#62) no longer exists")
    s.shot("02_box_after", "#277: back on the same box grid -- nothing was mounted")
    return s


def run_b64_import(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> list[gb_shots.Session]:
    """BACKLOG #64 review Fix 3 (F1 ruling): pdna_gen12_show_fused() now installs
    the SAME Option B streamed session pdna_gen12_show() does (source/pdna_gen12.c)
    -- this delta-gb image's own NV_GB import row IS emulator-testable proof of the
    read-parity fix now, not the still-HW-only pdna_gen12_show() FIL-streaming path
    (`app_pick_gb_save()`'s real SD file browser -- pdna_main.c:9391, the ONLY call
    site, `#ifndef PDNA_DELTA`, dead code on every image this harness can build;
    mGBA has no SD card at all -- see this function's own git-log history for the
    fuller citation this docstring used to carry before Fix 3 landed).

    BACKLOG #277 NOTE: the `import_*` leg below was RETIRED (the nested mount is closed by
    BACKLOG #239; that leg now pins the BANK ONLY refusal and the pairwise cmp is gone).
    The text that follows is the historical record.

    Captured Trainer / Bag-or-Pack / Flags / Dex / Map from BOTH of THIS image's
    real mounts over the SAME `which` save, named to `cmp` pairwise:
      - `import_*`  the nested NV_GB import (nav_to_gb_import(), THIS lane's own
                     streamed read-only session, ed == false, gs == &vw.s)
      - `direct_*`  the boot picker's own DIRECT GB row (boot_to_gb_session(), the
                     pre-existing resident-image mount, ed == true, gs == &g_ed->s)
    Same underlying save bytes either way -- a real pixel difference here would
    mean either the streamed session's read parity is wrong, or the read-only
    render draws different chrome than the editable one (worth a caption either
    way, not necessarily a bug: e.g. an edit cursor only the editable mount shows)."""
    kind_g1 = (which == "red")
    rows = [
        ("trainer",     3, GB_ART_COLD_SETTLE),   # own cold rom_gbui scan (run_u3_trainer)
        ("bag_or_pack", 7, GB_ART_COLD_SETTLE),   # own cold scan (run_u4_bag/run_u5_pack)
        ("flags",       8, gb_shots.BIG_SETTLE),  # no art fetch (run_b88_flags)
        ("dex",         6, gb_shots.BIG_SETTLE),  # BACKLOG #64 review R2: pdna_gbdex.c's own
                                                    # gate is `if (s->gen == GB_GEN2)` (not a
                                                    # Crystal-only check) -- gold ALSO lands on
                                                    # the "Pokedex"/"Unown forms" chooser first;
                                                    # only Gen 1 (red) skips it. Confirmed against
                                                    # source, not just run_b87_dex()'s own choice
                                                    # set (which never happened to cover gold).
        ("map",        16, GB_ART_COLD_SETTLE),   # own cold scan (run_m1_map/run_m1_map_gen2)
    ]
    sessions = []

    # ---- mount 1 (BACKLOG #277): the nested NV_GB import (streamed, ed == false) was
    # CLOSED by BACKLOG #239 (A on NV_GB from a live Gen-3 save = the "BANK ONLY"
    # refusal), so the import_* leg and the pairwise cmp are RETIRED. This leg pins
    # the refusal instead, so a reopened mount fails loudly.
    s1 = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b64_{which}_import_")
    print(f"== BACKLOG #64/#277: {which}.sav -- NV_GB import mount is retired -- refusal only ==")
    s1.run(700)
    s1.tap("A", settle=gb_shots.BIG_SETTLE)                # #68a boot picker, Emerald row (default) -> box
    nav_to_gb_refusal(s1, "01_refused",
                      f"#277: the nested NV_GB import that used to carry {which}.sav's "
                      "import_* half of the #64 read-parity compare is closed (BACKLOG "
                      "#239) -- A on GB import shows the \"BANK ONLY\" dialog")
    sessions.append(s1)

    # ---- mount 2: the boot picker's own direct GB row (resident image, ed == true)
    s2 = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b64_{which}_direct_")
    print(f"== BACKLOG #64 Fix 3: {which}.sav — boot picker's direct GB row (resident, ed==true) ==")
    boot_to_gb_session(s2, rom, which=which)
    for i, (tag, n, a_settle) in enumerate(rows):
        s2.tap("START", settle=gb_shots.BIG_SETTLE)
        s2.press_n("DOWN", n)
        s2.tap("A", settle=a_settle)
        if tag == "dex" and not kind_g1:
            s2.tap("A", settle=gb_shots.BIG_SETTLE)   # chooser row 0 (Pokedex) -> the real grid
            s2.run(2000)   # same header repaint settle as the import leg above
        s2.shot(f"{i + 1:02d}_{tag}",
                f"#64 Fix 3: {which}.sav via the boot picker's OWN direct GB row -- {tag} "
                "row -- the pre-existing resident-image mount (g_ed, editable), for a "
                "pixel `cmp` against the import mount's read-only render above")
        s2.tap("B", settle=gb_shots.BIG_SETTLE)
        if tag == "dex" and not kind_g1:
            s2.tap("B", settle=gb_shots.BIG_SETTLE)   # grid -> chooser -> box grid (2 levels)
        s2.run(2000)
    sessions.append(s2)

    return sessions


def run_standalone(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """pokedna-delta-gb.gba (the ONLY image now -- BACKLOG #68a retires the separate
    blank-flash `delta-gb-only` recipe): boot lands on gb_delta_boot_pick()'s BOOT
    PICKER (row 0 = the loaded Gen-3 save, rows 1..3 = Red/Gold/Crystal), because a
    Gen-3 save is ready (on a fresh emulator boot the flash is blank, so it comes from
    the fused .sav fallback) AND a GB corpus is fused in. Selecting a GB row
    (here: Red, one DOWN from the default Emerald selection) drives the SAME full
    read/write-capable STANDALONE mount tools/gb_shots.py's own run_gold()/run_red()
    drive against an SD-based single-fused-save image, but here backed by a REAL fused
    ROM, so RELEASE and CREATE (which that build can only ever refuse -- "Needs your
    Gen 1/2 ROM", no flashcart in mGBA) genuinely work.

    Uses Guy's own roms/gb corpus, which happens to have EVERY box on EVERY save
    completely full (a "living dex" style test save) -- there is no empty cell to
    CREATE into anywhere without first making one, so this drives RELEASE on one
    occupied cell first (safe: PDNA_DELTA's whole flash chip is a snapshot; gb_persist's
    D2/D5 in-session-only refusal means the release, like the later CREATE, is never
    actually written anywhere) and then CREATEs into the freed slot -- covering both
    D2/D5 (the refusal) and #68a's boot picker + the standalone mount it unlocks, in
    one continuous, honest session. Backing all the way out (B) reloads the Emerald
    save from flash and re-shows the SAME boot picker; picking its Emerald row from
    there continues into the ordinary Gen-3 box screen, where NV_GB's nested import is
    exercised too (see the end of this function) to prove it survived #68a untouched."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "dgb_boot_")
    print("== delta-gb (combined): boot picker, standalone mount, real CREATE, "
          "in-session refusal, then back to Emerald + NV_GB ==")

    # Session.__init__'s own 180-frame settle is calibrated against a plain single-
    # fused-save image; this combined build's Emerald.sav + the whole GB directory
    # boots slower (same reasoning as run_nav_gb's own s.run(700) below).
    s.run(700)
    s.shot("01_boot_picker", "#68a: gb_delta_boot_pick() -- the combined image has a Gen-3 "
                              "save ready (the fused .sav fallback; the flash is blank on a "
                              "fresh boot) AND a GB corpus fused in, so boot offers this picker instead of going "
                              "straight into the Emerald session: row 0 'Emerald.sav "
                              "(Gen 3)' (default selection) + one row per fused GB save "
                              "(Red/Gold/Crystal)")

    s.tap("DOWN", settle=gb_shots.SETTLE)                 # Emerald (row 0) -> Red (row 1)
    s.tap("A", settle=60)                                 # pick Red -> box grid (COLD fetch, gen1 loc empty; #279: no S1 info page)
    s.run(GB_ART_COLD_SETTLE)
    s.shot("03_box_grid", "#62 D1: box grid with REAL Red/Blue/Yellow art -- 4-shade Game Boy "
                           "sprites decoded from the fused Red.gb ROM, the first-ever cold scan "
                           "(D9: ~258 s of emulated GBA time) already ridden out")

    # This corpus's every box is full -- RELEASE the box's own first mon (top-left) to
    # open a slot CREATE can use. BACKLOG #198 item 4: this menu's row list predates
    # BACKLOG #93's DUPLICATE / TO DAY-CARE / EXPORT .pk rows -- app_mon_menu_readonly's
    # own occupied-cell order (source/pdna_main.c ~5290-5313, k_gb_ops_gen1's hooks:
    # .item=NULL Gen-1-only, .dup/.daycare/.export_one all set) is now VIEW/EDIT,
    # LEGALITY, MOVE TO BOX, COPY, DUPLICATE, TO DAY-CARE, EXPORT .pk, RELEASE, CANCEL --
    # a flat `DOWN x4` (the pre-#93 row count) now lands on DUPLICATE instead. Navigate
    # by ROW NAME (this constant's own .index(), not a bare magic number) so a future
    # row insertion/removal here breaks loudly (a ValueError) instead of silently
    # landing on the wrong row again.
    GB_STANDALONE_OCCUPIED_ROWS = [
        "VIEW/EDIT", "LEGALITY", "MOVE TO BOX", "COPY",
        "DUPLICATE", "TO DAY-CARE", "EXPORT .pk", "RELEASE", "CANCEL",
    ]
    down_to_release = GB_STANDALONE_OCCUPIED_ROWS.index("RELEASE")   # 7, not the old 4
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # cell menu
    s.press_n("DOWN", down_to_release, settle=gb_shots.SETTLE)   # -> RELEASE
    s.shot("04_release_menu", "#62 A3 (BACKLOG #198 item 4 recaption): the occupied-cell "
                               "menu the STANDALONE mount offers -- VIEW/EDIT, LEGALITY, "
                               "MOVE TO BOX, COPY, DUPLICATE, TO DAY-CARE, EXPORT .pk, "
                               "RELEASE, CANCEL (BACKLOG #93's three newer rows included; "
                               "the nested-import mount's own VIEW/LEGALITY/COPY/CANCEL, "
                               "plus every write action, since this session can actually "
                               "edit) -- cursor on RELEASE, the row this shot means to show",
                               claim=["RELEASE"])  # BACKLOG #184 retrofit: exactly the class
                               # of caption lie #198 item 4 found (a stale DOWN-count landed
                               # on DUPLICATE, not RELEASE) -- a mechanical claim is the floor
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # "Release this Pokemon?"
    s.shot("05_release_confirm", "#62: the release confirm dialog")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # A = yes -> gb_persist() -> PDNA_DELTA refusal
    s.shot("06_refusal", "#62 D2/D5: gb_persist()'s PDNA_DELTA branch -- 'Edits are in-session "
                          "only in the emulator build.' The release is NOT written anywhere "
                          "(there is no SD card, and this build's flash chip stays blank "
                          "either way), but D2's fix means it also is NOT lost: pristine is "
                          "re-baselined to the post-release image right here.")
    # BACKLOG #198 item 4 renav: dismissing the wall needed a LONGER settle than
    # BIG_SETTLE to finish repainting the box grid (found live: BIG_SETTLE's own 40
    # frames landed mid-repaint, a stale "RELEASED"-adjacent frame) -- 400 is what a
    # probe against this build confirmed settles it fully. Gen-1/2 boxes are LIST-
    # COMPACTED on delete (gbs_delete -> delete_from_list, source/gb_session.c:449),
    # unlike Gen 3's fixed-grid PC -- releasing the top-left mon (index 0) shifts
    # every later mon DOWN one index, so the newly-empty slot lands at the END of the
    # occupied range (index 19 of the original 20: row 3, col 1, six-column grid),
    # never at index 0 where SLOWBRO used to be. Confirmed live: without this nav,
    # the cursor (still at grid position 0,0) shows DUGTRIO (the mon that shifted
    # into slot 0), not an empty cell. DOWN x3 + RIGHT x1 from the cursor's post-
    # dismiss position (still slot 0) reaches index 19.
    s.tap("A", settle=400)                                 # dismiss -> back at the box, now 19/20
    s.press_n("DOWN", 3, settle=gb_shots.SETTLE)
    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.shot("07_empty_cell", "#62 (BACKLOG #198 item 4 renav): cursor on the freshly-"
           "released, now-empty cell (19/20) -- index 19 (row 3, col 1), the END of "
           "the compacted list, not index 0 where SLOWBRO (the released mon) used "
           "to be",
           claim=["19/20"])  # BACKLOG #184 retrofit: pdna_box.c's own
           # siprintf(bnocc, "%s  %d/%d", ...) banner

    s.tap("A", settle=gb_shots.BIG_SETTLE)                # empty-cell menu: EMPTY / CREATE / CANCEL
    s.shot("08_create_menu", "#62 A3: the empty-cell menu -- EMPTY (header) / CREATE / CANCEL")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # select CREATE -> pick_species(1)
    s.shot("09_species_picker", "#62: CREATE opens the same pdna_pick.c pick_species() screen "
                                 "Gen-3's own app_create_mon uses -- Bulbasaur (dex 1) "
                                 "pre-selected")

    s.tap("A", settle=60)                                 # pick Bulbasaur -> #265 origin choice
    s.shot("09b_origin_choice", "#265: BEFORE any ROM read, CREATE asks 'A = LEGIT COPY' "
                                 "(real level and moves, read from your ROM) or 'SELECT = FROM "
                                 "SCRATCH' (level 1 with Growl) -- a fused Red.gb counts as a "
                                 "registered ROM here, so the prompt shows",
           claim=["NEW POKEMON", "A = LEGIT COPY", "SELECT = FROM SCRATCH", "B = cancel"])
    s.tap("A", settle=60)                                 # A = LEGIT COPY -> gb_create_learn (ROM read)
    s.shot("10_busy_line", "#62 A3: 'Reading your ROM... This can take a moment.' -- unlike "
                            "gb_shots.py's SD-based CREATE test (which always refuses here, no "
                            "flashcart in mGBA), THIS build has a real fused Red.gb, so the "
                            "level/moveset lookup (rom_gblearn) genuinely runs")
    s.run(GB_ART_COLD_SETTLE)                             # gb_create_learn's own ROM read
    s.shot("11_new_mon_summary", "#62 A3: the NEW-mon summary -- a real 4-shade Game Boy "
                                  "portrait (decoded from the fused ROM, not a Gen-3 stand-in), "
                                  "Name/Lv/OT/ID/Status fields, EDIT mode (create-mode UX-parity "
                                  "with Gen 3's own create flow)")

    s.tap("START", settle=gb_shots.BIG_SETTLE)            # -> "Keep this Pokemon?"
    s.shot("12_keep_confirm", "#62 D5: 'A = write (session only)' / 'Kept for this session "
                               "only, not written to a card.' -- the gated wording from D5's "
                               "fix, replacing the SD half's 'backs up first' promise this "
                               "build cannot honour")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # A = write (session only) -> refusal
    s.shot("13_refusal_2", "#62 D2/D5: the same in-session-only refusal as 06 -- CREATE goes "
                            "through gb_persist() too")
    s.tap("A", settle=400)                # dismiss -> box grid, new mon showing (BACKLOG #198
                                            # item 4: same "BIG_SETTLE lands mid-repaint" bug
                                            # found live at frame 07's own dismiss -- 400 confirmed
                                            # live to settle this repaint fully too)
    s.shot("14_mon_in_grid", "#62 A3: the box grid re-paged with the newly created Bulbasaur "
                              "showing in the slot RELEASE freed -- CREATE end to end, on real "
                              "fused-ROM art, kept in-session per D2's fix")

    # 3x B: the post-CREATE grid still has a selection/cursor-mode layer or two to
    # unwind (calibrated by hand -- 1x B is enough from a box grid entered fresh, with
    # no create/release history) before pdna_box() itself returns 0 and the session
    # actually leaves.
    s.press_n("B", 3, settle=gb_shots.BIG_SETTLE)
    s.shot("15_back_at_picker", "#68a: backing all the way out of the standalone session (B) "
                                 "reloads the Emerald save from whichever source supplied it (the fused .sav here; the GB mount had "
                                 "overwritten g_save) and returns straight to "
                                 "gb_delta_boot_pick() -- the same boot picker as shot 01, not a "
                                 "dead end and not a separate blank-flash build")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                # B in the boot picker -> always = Emerald
    s.shot("16_emerald_box", "#68a: KEY_B on the boot picker always resolves to the Emerald row "
                              "-- with a valid flash save on hand there is no dead end to guard "
                              "against, unlike gb_delta_pick_save()'s blank-flash picker (KEY_B "
                              "there exits to nothing but itself). Lands in the ordinary Gen-3 "
                              "box screen, exactly as any delta build without a fused GB corpus "
                              "would boot straight to.")

    # BACKLOG #277: the nested NV_GB import this tail used to exercise (frames 17-18:
    # Gold's info page + the VIEW/LEGALITY/COPY/CANCEL menu) was CLOSED by BACKLOG #239.
    # Same navigation, from the SAME Emerald session #68a's picker just returned to,
    # now pins the "BANK ONLY" refusal (claim-checked) so a reopened mount fails loudly.
    nav_to_gb_refusal(s, "17_nv_gb_refused",
                      "#277 (was #68a's 17_nv_gb_info): from the Emerald session #68a's "
                      "boot picker just returned to, START > GB import shows the "
                      "\"BANK ONLY\" dialog (BACKLOG #239) instead of the nested "
                      "import's picker/info page")
    return s


def run_u2c_trainer(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """U2c (docs/GB-GAME-SCREENS-DESIGN.md sec 1.1): Red's OWN trainer card on the
    shared GB-screen shell, over the STANDALONE mount (boot picker DOWN -> Red row ->
    A -> box grid, #279: no info page), which rides out rom_gbsprite's cold scan the
    same way run_standalone()'s own 02_box_grid does. Then START -> nav menu, DOWN x3 ->
    Trainer -> A ->
    pdna_gbtrainer() -- Gen 1, so this lands on pdna_gbtrainer_gen1_card(), NOT the
    Emerald-art path tools/p1c_shots.py's run_gold_card() shoots (that one is Gen-2-
    only as of this slice). gbscr_open()'s own rom_gbui scan is a SEPARATE cold scan
    from rom_gbsprite's box-grid one (different locator, different ROM regions), so
    entering the card the first time gets its own GB_ART_COLD_SETTLE ride-out."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "u2c_")
    print("== U2c: Red's own trainer card (Red.sav, boot picker -> standalone -> Trainer) ==")

    boot_to_gb_session(s, rom, which="red")
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 3)                                     # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Trainer -> pdna_gbtrainer_gen1_card()
                                                              # (gbscr_open's OWN cold rom_gbui scan)
    s.shot("01_card_1to1", "U2c: Red's OWN trainer card, 1:1 centred at (40,8) -- "
                            "NAME/MONEY/TIME painted with the CARDFRAME block's frame "
                            "tiles for the panel border, the player pic (gb_sprite_gen1, "
                            "4 tile columns x 6 rows -- the real game's own text-box "
                            "border overwrites pic column 5 and row 7, D1 review fix), "
                            "and both 8-badge rows -- all shown OWNED (badge tiles, not "
                            "face tiles): Red.sav's own wObtainedBadges is 0xFF on this "
                            "save, all 8 badges genuinely owned")

    s.tap("SEL", settle=60)                                 # shell-wide toggle -> stretched
    s.shot("02_card_stretched", "U2c: SELECT stretches the SAME card to 240x160 -- "
                                 "the shell's own scale toggle (gb_scale_mode), not a "
                                 "card-specific key; the legend overlays the bottom row")

    s.tap("SEL", settle=60)                                 # back to 1:1
    s.shot("03_card_1to1_again", "U2c: SELECT again returns to 1:1 -- a live toggle")

    # Cursor cycle order (G1C_*): NAME(0) MONEY(1) TIME(2) then the 8 badges row-major.
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("04_cursor_money", "U2c: DOWN moves the cursor from NAME to MONEY")

    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("05_cursor_time", "U2c: DOWN again -> TIME")

    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("06_cursor_badge0", "U2c: DOWN again -> the first badge (row 1, col 0)")

    # D10 (review): Red.sav's badge0 starts OWNED (see shot 01's own caption), so A's
    # FIRST toggle here goes owned -> UNOWNED (face tile), not the other way around --
    # shots 07/08 were captioned backwards.
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # instant toggle -> UNOWNED
    s.shot("07_badge0_unowned", "U2c: A instantly toggles badge 0 to UNOWNED (it "
                                 "started owned, Red.sav's own wObtainedBadges = 0xFF) "
                                 "-- badge tile swaps to a face tile (never a palette "
                                 "change, design's own 'lit vs unlit is not a palette "
                                 "change' rule)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # toggle back -> OWNED
    s.shot("08_badge0_owned", "U2c: A again toggles it back to OWNED (badge tile)")

    s.press_n("UP", 3, settle=gb_shots.SETTLE)               # badge0 -> TIME -> MONEY -> NAME
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # A on NAME -> the identity WARNING first
    s.shot("09_editor_name", "U2c: A on NAME does NOT open the keyboard directly -- "
                              "gbtr_id_edit_ok() shows the identity-change WARNING "
                              "first ('Changes your TRAINER identity / Your own "
                              "Pokemon become traded', A=yes B=no), once per visit; "
                              "see shot 09b for the actual keyboard")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # confirm the warning -> osk_input opens
    s.shot("09b_editor_name_keyboard", "U2c: A (yes) on the warning -- NOW the "
                                        "EXISTING osk_input() pop-up opens (via "
                                        "gbtr_edit_row, the same sub-editor the plain "
                                        "page and Gen 2's card already share) -- "
                                        "PokeDNA's own on-screen keyboard, not a "
                                        "game-art dialogue")

    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                 # SELECT cancels osk_core (NOT B -- B is backspace in name editor)
    assert_screen(s, "gen1_trainer_card")
    s.shot("10_back_on_card", "U2c: SELECT cancels the name editor (osk_core's own cancel "
                              "key), back on the card, NAME selected, unchanged")

    # A REAL edit (badge0 toggled, LEFT changed) so B's commit prompt actually fires
    # -- stays in this SAME visit (never exits the card) rather than re-entering the
    # nav menu a second time, which left the game in an unexpected state (the nav
    # menu's own remembered row is not "Trainer" the way a fresh A-press assumes --
    # an earlier version of this script re-entered and landed on a mon's own
    # VIEW/EDIT menu instead; caught by looking at the screenshot, not by the
    # harness's own pixel-diff check, which only flags an IDENTICAL pair).
    s.press_n("DOWN", 3, settle=gb_shots.SETTLE)               # NAME -> MONEY -> TIME -> badge0
    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # toggle badge0 -- a REAL change
    assert_screen(s, "gen1_trainer_card")
    s.shot("11_real_edit", "U2c: badge0 toggled and LEFT changed (unlike shots "
                            "06-08's toggle-and-back, this one stays changed)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                     # B -> the actual commit prompt
    s.shot("12_commit_prompt", "U2c: B with a real pending edit -> "
                                "'Save trainer changes?' (app_confirm), the SAME "
                                "dialog Gen 3's own card_editor uses -- shot 10's "
                                "own B (nothing changed, t == t0) instead took the "
                                "silent memcmp no-op path straight back to the "
                                "card, which is why this shot needed a real edit")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                     # decline -- discard the real edit too
    assert_screen(s, "gb_box_grid")
    s.shot("13_declined", "U2c: declining a REAL edit also discards it -- the "
                           "save was never written (gbt_write never ran), back "
                           "at the box grid")
    return s



def run_u3_trainer(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """U3 (BACKLOG #66, docs/GB-GAME-SCREENS-DESIGN.md sec 1.2): Gold/Silver/Crystal's
    OWN trainer card on the shared GB-screen shell -- the Gen-2 sibling of
    run_u2c_trainer() above. Boot picker DOWN x(gb_save_pick_index(rom)[which]+1) ->
    `which` row -> A -> box grid (#279: no info page, cold rom_gbsprite scan) -> START ->
    nav menu -> DOWN x3 -> Trainer -> A -> pdna_gbtrainer_gen2_card() (gbscr_open()'s
    OWN separate cold rom_gbui scan, same GB_ART_COLD_SETTLE ride-out
    run_u2c_trainer() needs)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"u3_{which}_")
    print(f"== U3: {which}'s own trainer card (boot picker -> standalone -> Trainer) ==")

    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box grid -> nav menu
    s.press_n("DOWN", 3)                                       # Party -> Bank -> Daycare -> Trainer
    s.tap("A", settle=GB_ART_COLD_SETTLE)                    # Trainer -> pdna_gbtrainer_gen2_card()
                                                                # (gbscr_open's OWN cold rom_gbui scan)
    s.shot("01_page1_1to1", f"U3: {which}'s own trainer card, page 1, 1:1 -- NAME/ID No/"
                             "MONEY, the 5x7 card pic (row-major on Gold, column-major on "
                             "Crystal via rom_gbui_tile's own colmajor reorder), the STATUS "
                             "strip, #DEX (dex_owned popcount), PLAY TIME (colon currently "
                             "off-phase), and the BADGES(r) page hint")

    s.tap("SEL", settle=60)                                   # shell-wide toggle -> stretched
    s.shot("02_page1_stretched", "U3: SELECT stretches the SAME page to 240x160 -- the "
                                  "shell's own scale toggle, not a card-specific key")
    s.tap("SEL", settle=60)                                   # back to 1:1
    s.shot("03_page1_1to1_again", "U3: SELECT again returns to 1:1")

    # Cursor cycle order page 1 (G2C_*): NAME(0) ID(1) MONEY(2) TIME(3).
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("04_cursor_id", "U3: DOWN moves the cursor from NAME to ID No.")
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("05_cursor_money", "U3: DOWN again -> MONEY")

    s.tap("UP", settle=gb_shots.SETTLE)                        # MONEY -> ID
    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # A on ID -> identity WARNING first
    s.shot("06_editor_id_warning", "U3: A on ID No. shows the identity-change WARNING "
                                    "first (gbtr_id_edit_ok(), the SAME sub-editor gate "
                                    "Gen 1's card and the plain page already share) -- "
                                    "ID has no cursor slot on Gen 1's own card at all, "
                                    "new to a GB card here")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # confirm -> num_entry opens
    s.shot("06b_editor_id_popup", "U3: A (yes) on the warning -- num_entry()'s existing "
                                   "pop-up opens (GBTR_ID, the same sub-editor "
                                   "pdna_gbtrainer_plain's own ID row uses); this is "
                                   "osk_search()'s own search-style keyboard, not a "
                                   "numeric keypad -- B is DELETE here (osk_core's own "
                                   "legend), SELECT cancels, START confirms")
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                   # SELECT cancels osk_core (NOT B -- B deletes)
    s.shot("07_back_on_card", "U3: SELECT cancels the ID editor (osk_core's own cancel "
                               "key), back on page 1, ID selected, unchanged")

    # RIGHT flips to page 2 -- unlike Gen 1's card, this key pages rather than
    # cycling badges (the real game's own binding, design sec 1.2).
    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.shot("08_page2_1to1", "U3: RIGHT flips to page 2 -- the BADGES strip and the 8 "
                             "gym-leader faces (10 tiles each, 4 across the top row then "
                             "3+3 below); cursor lands on badge 0")

    s.tap("SEL", settle=60)
    s.shot("09_page2_stretched", "U3: page 2 stretched to 240x160")
    s.tap("SEL", settle=60)

    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # instant toggle badge 0
    s.shot("10_badge0_toggled", "U3: A instantly toggles badge 0 (the accepted "
                                 "deviation: a STATIC 2x2 BADGES-block overlay over the "
                                 "face's centre when owned -- the real game draws this "
                                 "as an animated OAM sprite the BG-tilemap oracle used "
                                 "to build this card cannot see at all)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # toggle back (no residual edit)
    s.shot("11_badge0_back", "U3: A again toggles badge 0 back")

    s.tap("LEFT", settle=gb_shots.SETTLE)                      # back to page 1
    s.shot("12_page1_again", "U3: LEFT flips back to page 1")

    # A REAL edit (a Johto badge left toggled) so B's commit prompt actually fires --
    # same reasoning as U2c's own shots 11/12/13: stays in this SAME visit rather than
    # re-entering the nav menu, which does not reliably land back on Trainer.
    s.tap("RIGHT", settle=gb_shots.SETTLE)                     # page 1 -> page 2
    s.tap("A", settle=gb_shots.BIG_SETTLE)                     # toggle badge 0 -- a REAL change
    s.shot("13_real_edit", "U3: badge 0 toggled and LEFT changed (unlike shots 10/11's "
                            "toggle-and-back, this one stays changed)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                     # B -> the actual commit prompt
    s.shot("14_commit_prompt", "U3: B with a real pending edit -> 'Save trainer "
                                "changes?' (app_confirm), the SAME dialog Gen 1's own "
                                "card and Gen 3's own card_editor use")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                     # decline -- discard the real edit too
    # The trainer screen (a nav-menu item, reached over the box grid) leaves an extra
    # UI layer or two behind it on the way back down -- same reasoning as
    # run_standalone()'s own "3x B to fully unwind" comment for its post-CREATE grid.
    # One extra B here is enough by hand-calibration; verified by shot 15 itself
    # showing the bare box grid, not a mon submenu.
    s.tap("B", settle=gb_shots.BIG_SETTLE)
    s.shot("15_declined", "U3: declining a REAL edit also discards it -- the save was "
                           "never written (gbt_write never ran), back at the box grid")

    # Re-enter the card fresh for the START/plain-page shots (B on the front page above
    # exits the card entirely, same contract as U2c's own B).
    s.tap("START", settle=gb_shots.BIG_SETTLE)                 # box grid -> nav menu
    s.press_n("DOWN", 3)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                      # Trainer -> the card again
    s.tap("START", settle=gb_shots.BIG_SETTLE)                 # -> the plain field-complete page
    s.shot("16_plain_page", "U3: START reaches the full plain row-list page (coins, "
                             "mom's money/save, rival, mother, Kanto badges -- everything "
                             "not on the card face itself, unchanged data model)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                     # B on the plain page: discard, no commit
    s.shot("17_after_plain_back", "U3: B on the plain page discards (P1b's own rule) -- "
                                   "back at the box grid, not back on the card (the plain "
                                   "page is its own top-level fallback screen)")
    return s


def run_u4_bag(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """U4 (BACKLOG #67, docs/GB-GAME-SCREENS-DESIGN.md sec 1.3): Red/Yellow's OWN
    Item bag + PC store on the shared GB-screen shell. `rom` must be a ONE-Gen-1-ROM
    fused image (tools/fuse_gb.py fed only Red.gb+Red.sav, or only Yellow.gb+
    Yellow.sav -- BACKLOG #98's known harness gap: the fused image lookup is keyed by
    GENERATION only, so a Red+Yellow-both image would serve whichever ROM the cache
    happens to answer with, not deterministically the one this run asked for). BACKLOG
    #118 step 0: a single-fused-GB-save image with no Emerald.sav SKIPS the "PICK A
    SAVE" picker entirely (gb_delta_pick_save's `if (n == 1) return 0;`,
    pdna_main.c:8567) -- this docstring previously (wrongly) described a one-row
    picker here; boot_to_gb_session() now detects whichever shape `rom` actually is
    at runtime instead of assuming.

    Nav: boot_to_gb_session() (picker skipped on this image shape) -> box grid
    (rom_gbsprite cold scan) -> START -> nav menu -> DOWN x7 (Party->Bank->Daycare->
    Trainer->Clock fix->Mirage->Pokedex->Bag, PDNA_NAV_ITEMS index 7) -> A ->
    pdna_gbbag() -- Gen 1, so this lands on pdna_gbbag_gen1_screen() (gbscr_open()'s
    own cold rom_gbui scan, separate cache from rom_gbsprite's box-grid one)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"u4_{which}_")
    print(f"== U4: {which}'s own Item bag (boot picker -> standalone -> Bag) ==")

    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 7)                                     # Party -> ... -> Bag (index 7)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Bag -> pdna_gbbag_gen1_screen()
                                                              # (gbscr_open's OWN cold rom_gbui scan)
    s.shot("01_items_row1", f"U4: {which}'s OWN Item bag, 1:1 centred -- the TEXTBOX "
                             "block's frame tiles (25-31, not tile 0 -- U2a's own "
                             "finding, re-confirmed here), 4 item rows visible (NOT "
                             "3 -- the design doc's own 'wMaxMenuItem=2' guess was "
                             "wrong), cursor on row 1 (the first entry)")

    s.tap("SEL", settle=60)                                     # shell-wide toggle -> stretched
    s.shot("01b_stretched", "U4: SELECT stretches the same list to 240x160 -- the "
                             "shell's own scale toggle, not a bag-specific key")
    s.tap("SEL", settle=60)                                     # back to 1:1
    s.shot("01c_1to1_again", "U4: SELECT again returns to 1:1")

    if which == "red":
        s.tap("DOWN", settle=gb_shots.SETTLE)
        s.shot("02_items_row2", "U4: DOWN moves the cursor to row 2")
        s.tap("DOWN", settle=gb_shots.SETTLE)
        s.shot("03_items_row3", "U4: DOWN again -> row 3")
        s.tap("DOWN", settle=gb_shots.SETTLE)                # row 3 -> row 4 (still visible)
        s.tap("DOWN", settle=gb_shots.SETTLE)                # row 4 -> scrolls: the window
                                                                # shifts down one row per press
        s.shot("04_scrolled", "U4: DOWN past the 4th visible row scrolls the window; "
                               "the cursor stays PINNED at the 3rd visible row (D3, "
                               "U4 review) and a down-scroll marker (font tile 0xEE) "
                               "BLINKS at (18,11) while more of the list is below -- it "
                               "is a real, visible tile (an earlier draft of this "
                               "caption claimed no marker exists at all; it does, this "
                               "shot's own blink phase just happened to land OFF)")

        s.tap("LEFT", settle=gb_shots.BIG_SETTLE)             # Items -> the PC store (same box)
        s.shot("05_pc_store", "U4: LEFT/RIGHT switches Items <-> the PC item store -- "
                               "the IDENTICAL box/list routine over a different pocket "
                               "(design doc's own citation: 'ITEMLISTMENU again, over "
                               "wNumBoxItems'); NOT independently pixel-dumped against "
                               "the real cart this slice (reaching a Pokemon Center PC "
                               "needs overworld navigation, out of this slice's time box)")
        s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)            # back to Items
        s.shot("05b_back_to_items", "U4: RIGHT switches back to Items")

        s.tap("A", settle=gb_shots.BIG_SETTLE)                # A on the selected item -> qty editor
        s.shot("06_qty_editor", "U4: A on the selected item opens the existing "
                                 "numeric editor (num_entry, 1-99) -- the SAME pop-up "
                                 "every other GB screen's own field edits use, not a "
                                 "game-art dialogue")
        s.tap("SEL", settle=gb_shots.BIG_SETTLE)              # osk_search: SELECT cancels
                                                                # (B is backspace, NOT cancel --
                                                                # p1c_shots.py's own precedent)

        s.tap("START", settle=gb_shots.BIG_SETTLE)            # -> the ITEM MENU (ADD/REMOVE/SWAP)
        s.shot("07_item_menu", "U4: START opens the item menu -- ADD ITEM / REMOVE / "
                                "SWAP / CANCEL; SWAP does NOT swap the row it was "
                                "opened on with the next one -- it ARMS a mark on "
                                "that row (the list's own 0xED cursor glyph, D10) and "
                                "returns to the list, where the game itself works: "
                                "move the cursor to a second row and A swaps the two, "
                                "or B drops the mark with nothing moved (see the "
                                "armed-SWAP shots below)")
        # BACKLOG #195: ADD ITEM now opens the real item picker (pick_item(),
        # restricted to GBIN_GEN1 with the Gen-1 ceiling) instead of a raw numeric
        # "ITEM ID" prompt -- real names, a category filter (All/Items/TM-HM,
        # gbb_pocket_of()), list-view navigation. The picker's own admission test
        # (item_build()'s ceiling) means an out-of-range id (e.g. the old "251"
        # demo) can no longer even be SELECTED, so that refusal demo is gone from
        # this chain -- BAD ID/BAD QUANTITY are still exercised by
        # tests/host_gbbag_test.c's own gbb_insert() unit coverage (unchanged by
        # this lane), not a UI demo here anymore. QUANTITY itself is still the
        # SAME raw num_entry_opt prompt as before (BACKLOG #195 F2 only replaced
        # the id half), so that half of the old tap recipe (B clear seeded "1",
        # digit-row RIGHT/A, START confirm) is reused verbatim below.
        s.tap("A", settle=gb_shots.BIG_SETTLE)                # ADD ITEM -> pick_item() opens
        s.shot("08_picker_open", "U4 (BACKLOG #195): ADD ITEM now opens the real "
                                  "item picker -- REAL Gen-1 names (gb_item_label, "
                                  "not '#n'), a category header ('ITEM [All] 251': "
                                  "251 = ids 0..250, gbb_max_item_id(RED)=250 "
                                  "inclusive), list view, cursor on MASTER BALL "
                                  "(pick_item(1)'s own `current` -- '#0' NO_ITEM's "
                                  "own row is visible just above it, still "
                                  "selectable, gbb_insert() refuses it as BAD ID "
                                  "the same as before)")

        s.tap("START", settle=gb_shots.BIG_SETTLE)            # -> ritem_cat_menu (BACKLOG #195 F1)
        s.shot("08b_category_menu", "U4: START opens the restricted picker's OWN "
                                     "category menu -- Gen 1 offers only All/"
                                     "Items/TM-HM (gbb_pocket_of()'s own Gen-1 "
                                     "contract: no separate Key/Balls pocket on "
                                     "this cartridge), cursor on 'All'")
        s.tap("DOWN", settle=gb_shots.SETTLE)                 # All -> Items
        s.tap("A", settle=gb_shots.BIG_SETTLE)                # pick Items -> re-filters, closes menu
        s.shot("08c_items_filtered", "U4: picking 'Items' re-filters the list "
                                      "through gbb_pocket_of() -- the header now "
                                      "reads 'ITEM [Items]', and id 0 (NO_ITEM, "
                                      "invalid for every pocket) is gone from the "
                                      "top of the list -- MASTER BALL is now row 0")

        # Navigate to POTION (id 20/0x14): 19 DOWN presses from MASTER_BALL (id 1,
        # now row 0 under the Items filter) -- ids 1..20 have no TM/HM ids in
        # range (Gen 1's TM/HM block starts at 0xC4/196) and no other exclusion,
        # so the Items-filtered list and the unfiltered list agree on this stretch;
        # 19 DOWNs is exactly "one row per id from 1 to 20".
        s.press_n("DOWN", 19, settle=gb_shots.SETTLE)
        s.shot("08d_potion_selected", "U4: 19 DOWNs from MASTER_BALL lands on "
                                       "POTION (id 20) -- a real name from the "
                                       "table, not '#20'")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                # pick POTION -> QUANTITY prompt (unchanged UI)
        s.shot("08e_add_item_qty", "U4: picking an item goes straight to the SAME "
                                    "quantity prompt as before (num_entry_opt, "
                                    "1-99) -- BACKLOG #195 F2 only replaced the id "
                                    "half of ADD ITEM")
        s.tap("B", settle=gb_shots.SETTLE)                    # clear the seeded "1"
        s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)         # digit row: col0 '1' -> col8 '9'
        s.tap("A", settle=gb_shots.SETTLE)                    # type '9' -> field "9"
        s.tap("A", settle=gb_shots.SETTLE)                    # type '9' again (cursor unmoved) -> "99"
        s.tap("START", settle=gb_shots.BIG_SETTLE)            # confirm qty=99 -> gbb_insert(...,20,99): fresh slot
        s.shot("09_potion_added", "U4 (BACKLOG #195, 'Gen 1 add -> row appears'): "
                                   "POTION x99 -- a brand-new row, appended at the "
                                   "list's own last slot (ADD ITEM's own "
                                   "'*sel = l->count-1' rule, unchanged), through "
                                   "the picker end to end")

        # N4's own saturation-refusal demo, reused verbatim except for HOW the id
        # is chosen: ADD ITEM POTION again (a fresh pick_item() call always opens
        # at cat=All, current=1 -- the Gen-1 ADD site never calls
        # pick_item_set_gen1_2_cat(), so this is the SAME 19-DOWNs-from-MASTER_BALL
        # trip as above, just over the unfiltered 251-row list instead of the
        # 250-row Items-filtered one -- id 0's own extra row exactly cancels out
        # id 20 also shifting up by one, so the DOWN count is unchanged).
        s.tap("START", settle=gb_shots.BIG_SETTLE)            # -> item menu, cursor still on POTION (last row)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                # ADD ITEM -> picker opens fresh (cat=All again)
        s.press_n("DOWN", 19, settle=gb_shots.SETTLE)         # MASTER_BALL -> POTION (All list)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                # pick POTION again -> QUANTITY prompt
        s.tap("B", settle=gb_shots.SETTLE)                    # clear seeded "1"
        s.press_n("RIGHT", 4, settle=gb_shots.SETTLE)         # col0 -> col4 '5'
        s.tap("A", settle=gb_shots.SETTLE)                    # type '5' -> field "5"
        s.tap("START", settle=gb_shots.BIG_SETTLE)            # confirm qty=5 -> gbb_insert(...,20,5): MERGE path
        s.shot("09b_saturation_refusal", "U4: merging qty 5 into POTION (already "
                                          "at the 99 cap from the ADD above) "
                                          "saturates and refuses -- gbb_insert() "
                                          "SETS the existing stack to the cap (99) "
                                          "and returns GBB_ERR_QTY (gb_bag.c's own "
                                          "'sum > cap' branch); gbbag_start_menu's "
                                          "own msg_wait('SATURATED', ...) reports "
                                          "it")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                # dismiss the msg_wait -- back on the list
        s.shot("09c_after_add", "U4: after dismissing the refusal, the cursor is "
                                 "already on POTION's own row (ADD ITEM's own "
                                 "'*sel = last entry' rule) -- it reads x99, "
                                 "confirming the saturating write landed exactly "
                                 "where it started (99 -> 99)")

        # BACKLOG #195 re-derivation (measured, not assumed): the OLD chain here
        # used to REMOVE the just-added POTION before leaving, on the theory that
        # the saturation merge above made no byte change so a further edit was
        # needed to reach the commit prompt. Verified against the real screen
        # (throwaway diagnostic script, not shipped): ADD-then-REMOVE of the SAME
        # freshly-appended slot is mathematically a NO-OP against t0 (gbb_remove()
        # zeros the vacated tail slot, restoring the decoded model byte-for-byte),
        # so that old B afterward silently fell into the NO-OP path, not the
        # commit prompt -- leaving straight after POTION's own ADD (no REMOVE) is
        # the one edit still pending relative to t0, and IS what actually reaches
        # 'Save bag changes?' below.
        s.tap("B", settle=gb_shots.BIG_SETTLE)                # B -> the commit prompt (POTION's ADD is still pending)
        s.shot("10_commit_prompt", "U4: B with a real pending edit (POTION x99, "
                                    "still unsaved) -> 'Save bag changes?' "
                                    "(app_confirm), the same dialog every other "
                                    "GB screen's own commit uses")
        s.tap("B", settle=gb_shots.BIG_SETTLE)                # decline -- discard the edit
        s.shot("11_declined", "U4: declining discards the edit -- gbb_write never ran, "
                               "back at the box grid")

        # N6(a): the armed-SWAP state (D10) -- START > SWAP on a row ARMS a mark,
        # it does not swap on the spot. Re-enter the bag screen (declining above
        # never persisted anything, so gbb_read() below re-reads the ORIGINAL
        # unedited pocket -- POTION is gone again, row 0/row 1 are back to their
        # pristine ids). None of this touches pick_item() at all -- reused
        # verbatim from before BACKLOG #195.
        # D-reentry (this pass, empirically): BIG_SETTLE alone is NOT enough idle
        # time for the box grid to accept a fresh START right after RETURNING
        # from the bag screen -- measured with a throwaway diagnostic script: the
        # very next START press was silently swallowed (no menu opened, same
        # frame) with only BIG_SETTLE=40 frames of idle first, but succeeded with
        # 250. The FIRST entry into the bag screen (this function's own opening
        # sequence, above) never hits this because it follows a `run(GB_ART_COLD_
        # SETTLE)` (32,000 frames) already, not a bare BIG_SETTLE return.
        s.run(250)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
        s.press_n("DOWN", 7, settle=gb_shots.SETTLE)            # Party -> ... -> Bag (index 7)
        s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Bag -> pdna_gbbag_gen1_screen()
        s.shot("12_reentry_pristine", "U4: N6(a) setup -- re-entering the bag screen "
                                       "after declining shows the ORIGINAL, unedited "
                                       "Items list (POTION was never written)")
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> item menu, cursor on row 0
        s.press_n("DOWN", 2, settle=gb_shots.SETTLE)            # ADD ITEM -> REMOVE -> SWAP (csel 2)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # SWAP: arms row 0 as swap_src, returns
        s.tap("DOWN", settle=gb_shots.SETTLE)                   # move the cursor OFF the source row --
                                                                 # only now does row 0's own 0xED marker
                                                                 # stop being hidden behind is_sel
        s.shot("12_swap_armed", "U4: SWAP arms row 0 (the mark glyph -- the list's "
                                 "own 0xED cursor tile, D10 -- stays lit on row 0) "
                                 "and returns to the list; the cursor itself is now "
                                 "on row 1 (its OWN 0xED), so TWO rows show the same "
                                 "glyph at once -- this is the armed state, not a "
                                 "swap that already happened")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the destination -> the actual swap
        s.shot("12b_swap_done", "U4: A on the destination (row 1) performs the swap "
                                 "-- row 0 and row 1's entries have traded places; "
                                 "the mark is gone (g1_swap_active cleared) and "
                                 "neither row shows a stray 0xED anymore")

        # Undo this real edit so the B-cancel proof below starts from a clean diff
        # baseline: swap row 0/row 1 back, then leave. The cursor is CURRENTLY on
        # row 1 (12b_swap_done's own A-on-destination left `sel` there, unchanged
        # by the swap itself) -- SWAP arms whatever row the cursor is ON, so
        # arming here grabs row 1 (post-swap) as the source; move UP to row 0 as
        # the destination, not DOWN, to swap the SAME pair back.
        s.tap("START", settle=gb_shots.BIG_SETTLE)
        s.press_n("DOWN", 2, settle=gb_shots.SETTLE)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # re-arm SWAP on row 1 (cursor's current row)
        s.tap("UP", settle=gb_shots.SETTLE)                     # cursor -> row 0 (the OTHER half of the pair)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # swap back -> pristine order restored
        s.tap("B", settle=gb_shots.BIG_SETTLE)                  # -> the no-op path: two swaps of the SAME
                                                                 # pair restore identical bytes, memcmp(bag,t0)
                                                                 # == 0 again, straight back to the box grid
        s.shot("12c_swap_undone", "U4: swapping row 0/row 1 back and leaving -- two "
                                   "swaps of the same pair restore the identical "
                                   "bytes (memcmp(bag,t0)==0), so this is the SAME "
                                   "silent no-op path as an unedited visit, straight "
                                   "back to the box grid, no confirm dialog")

        # N6(a) part 2: B drops an armed mark WITHOUT moving anything.
        s.run(250)                                              # re-entry idle (see the D-reentry note above)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
        s.press_n("DOWN", 7, settle=gb_shots.SETTLE)            # Party -> ... -> Bag (index 7)
        s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Bag -> pdna_gbbag_gen1_screen()
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> item menu, cursor back on row 0
        s.press_n("DOWN", 2, settle=gb_shots.SETTLE)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # arm SWAP on row 0
        s.tap("DOWN", settle=gb_shots.SETTLE)                   # cursor -> row 1 (source's mark visible)
        s.shot("13_swap_armed_again", "U4: N6(a) part 2 setup -- SWAP armed on row 0 "
                                       "again, cursor moved to row 1")
        s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B: drop the mark, nothing moves
        s.shot("13b_mark_dropped", "U4: B drops the armed mark -- row 0's own 0xED "
                                    "is gone, row 1 still shows the cursor's own "
                                    "0xED, and NEITHER row's underlying entry "
                                    "changed (compare against 12_swap_armed above: "
                                    "same ids, same quantities, same order)")
        s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B again (nothing armed) -> leave
        s.shot("13c_left_no_prompt", "U4: leaving right after -- straight back to "
                                      "the box grid, no 'Save bag changes?' prompt "
                                      "at all -- that prompt only ever fires when "
                                      "memcmp(bag,t0)!=0; its absence here IS the "
                                      "byte-compare proof for the B-cancel-mark "
                                      "path")

        # BACKLOG #195 F2, "TM add": ADD ITEM through the picker's OWN TM-HM
        # category -- picking a TM/HM item sets it via the SAME gbb_insert() path
        # a Gen-1 Items entry uses (Gen 1 has no separate TM/HM pocket at all,
        # "TMs are bag items", gb_bag.h's own note) -- the category filter is a
        # PICKER-side convenience, not a different storage/routing rule the way
        # Gen 2's ADD ITEM needs (that is BACKLOG #195 F2's OTHER half, u5_pack
        # below).
        s.run(250)                                              # re-entry idle (see the D-reentry note above)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
        s.press_n("DOWN", 7, settle=gb_shots.SETTLE)            # Party -> ... -> Bag (index 7)
        s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Bag -> pdna_gbbag_gen1_screen()
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> item menu, cursor row 0 (ADD ITEM)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> picker opens (cat=All)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> category menu
        s.press_n("DOWN", 2, settle=gb_shots.SETTLE)            # All -> Items -> TM-HM
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick TM-HM -> re-filters, closes menu
        s.shot("14_tmhm_filtered", "U4 ('TM add'): the TM-HM category (gbb_pocket_"
                                    "of()'s Gen-1 TM/HM range, 0xC4-0xFA) filters "
                                    "the SAME picker to just those ids -- header "
                                    "'ITEM [TM-HM]', first row HM01 (id 0xC4, "
                                    "Gen 1's HM01-05 come before TM01-50 in id "
                                    "order -- gb_item_names.c's own gb1_tmhm_label "
                                    "range), a SYNTHESIZED name, not a table entry")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick HM01 -> QUANTITY prompt
        s.shot("14b_tm_qty_prompt", "U4: picking a TM/HM item goes through the "
                                     "SAME quantity prompt as any other Gen-1 item "
                                     "-- Gen 1 has no count-array pocket, HM01 is "
                                     "just another Items-pocket entry with a real "
                                     "(synthesized) name")
        # B cancels this pick (HM01 is a Gen-1 KEY item -- gbb_is_g1_key_item()'s
        # own "HM01-05 are always key items in both games" fact -- the list's OWN
        # row paint hides the quantity column for those, gbbag_row_paint's real
        # rule, unrelated to BACKLOG #195; picking TM01 instead keeps this demo
        # showing an ordinary '×N' row like every other add above) and moves 5
        # rows down (HM01..HM05, 5 rows) to TM01 (id 0xC9), an ordinary TM row.
        s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # osk_search's own CANCEL key (SELECT, not B --
                                                                 # B is backspace) -- num_entry_opt() returns
                                                                 # false, aborting the whole ADD; gbbag_start_
                                                                 # menu's own `continue` redraws ITEM MENU,
                                                                 # csel still 0 (ADD ITEM) -- no re-open needed
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM again -> picker opens fresh (cat=All)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> category menu
        s.press_n("DOWN", 2, settle=gb_shots.SETTLE)            # All -> Items -> TM-HM
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick TM-HM -> filtered list, cursor on HM01
        s.press_n("DOWN", 5, settle=gb_shots.SETTLE)            # HM01..HM05 (5 rows) -> TM01
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick TM01 -> QUANTITY prompt
        s.tap("B", settle=gb_shots.SETTLE)                      # clear seeded "1"
        s.press_n("RIGHT", 2, settle=gb_shots.SETTLE)           # col0 -> col2 '3'
        s.tap("A", settle=gb_shots.SETTLE)                      # type '3' -> field "3"
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=3 -> gbb_insert(...,0xC9,3): fresh slot
        s.shot("15_tm_added", "U4 ('TM add', BACKLOG #195): TM01 x3 -- a brand-new "
                               "row with its SYNTHESIZED name (gb1_tmhm_label) AND "
                               "a real quantity column (a TM, unlike HM01 above, "
                               "is not a Gen-1 key item), appended at the list's "
                               "own last slot, through the picker's TM-HM category "
                               "end to end")
        s.tap("B", settle=gb_shots.BIG_SETTLE)                  # -> commit prompt (a real edit pending)
        s.tap("B", settle=gb_shots.BIG_SETTLE)                  # decline -- discard the edit (leave the
                                                                 # fixture's own pristine bag for any later run)
        s.shot("16_tm_declined", "U4: declining the TM add discards it -- gbb_write "
                                  "never ran, back at the box grid, fixture stays "
                                  "pristine for a later run of this script")

    return s


def run_u4_empty(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """N6(e): Red's own Item bag with an EMPTY Items pocket (count 0) -- `rom` must
    be fused with a Red save whose Items pocket was zeroed by docs/shots/rvu4/
    mkbag.c (no ids given on argv -- gbb_write() then persists count=0). BACKLOG
    #118 step 0: boot_to_gb_session() decides at runtime whether the "PICK A SAVE"
    picker is even showing (a single-fused-GB-save image with no Emerald.sav
    skips it, pdna_main.c:8567) -- same nav as run_u4_bag()'s own red case
    otherwise: box grid -> START -> nav DOWN x7 -> A."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "u4_empty_")
    print("== N6(e): Red's own Item bag, Items pocket count == 0 ==")
    boot_to_gb_session(s, rom, which="red")
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 7)
    s.tap("A", settle=GB_ART_COLD_SETTLE)
    s.shot("01_empty_pocket", "N6(e): Items pocket count == 0 -- the list is CANCEL "
                               "alone (D3's own '+ the CANCEL row' rule over a "
                               "0-entry pocket: total = 0 + 1 = 1 row), cursor "
                               "pinned on it. Byte-for-byte oracle proof: the real "
                               "cartridge on the SAME save (docs/shots/rvu4/"
                               "oracle.py --tag red_empty) reads grid[4][5] == "
                               "0xed (the cursor glyph) and grid[4][6..11] == "
                               "0x82,0x80,0x8d,0x82,0x84,0x8b ('C','A','N','C','E',"
                               "'L') at the SAME screen position -- see this run's "
                               "own paste in the U4 report.")
    return s


def run_m1_map(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """M1 (BACKLOG #91, docs/GB-MAP-DESIGN.md): Red's OWN current-map view,
    read-only, on the shared GB-screen shell. `rom` must be a Red-only fused image
    (tools/fuse_gb.py fed only Red.gb+Red.sav -- BACKLOG #98's fused-image-by-
    generation-only gap, same reason run_u4_bag()/run_u5_pack() require a single-
    ROM image).

    Nav: boot picker DOWN -> A -> A -> box grid (rom_gbsprite cold scan) -> START
    -> nav menu -> DOWN x16 (Party->Bank->Daycare->Trainer->Clock fix->Mirage->
    Pokedex->Bag->Flags & counters->Bases->Blocks->Tickets->Records->Frontier->
    Fly->Contests->Map, PDNA_NAV_ITEMS index 16) -> A -> pdna_gbmap_gen1() (gbscr_
    open()'s own cold rom_gbui scan, separate cache from rom_gbsprite's box-grid
    one, PLUS rom_gbmap.c's own separate locate pass over the same ROM).

    m1 review D6/D1 correction: L/R are the shell's own SIZE toggle here (same as
    SELECT) -- the D-pad ALONE pans. wXCoord/wYCoord (Red.sav's real x=6/y=4) are
    ONE halving from a block (gbmap_block_of(), rom_gbmap.h), giving block (3,2),
    NOT (1,2) -- an earlier draft of this file said the player's real x already
    sat at the west clamp (vbx=0); with the corrected halving it does not: width 7
    - the 5-block viewport = 2 steps of slack, and the marker's own block (3) is
    NOT at either edge, so the initial vbx is 1 (clampi(3 - 5//2, 0, 2)), one step
    of room on EACH side."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "m1_map_")
    print("== M1: Red's own current-map view (boot picker -> standalone -> Map) ==")

    boot_to_gb_session(s, rom, which="red")
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box grid -> nav menu
    s.press_n("DOWN", 16)                                     # Party -> ... -> Map (index 16)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                    # Map -> pdna_gbmap_gen1()
    s.shot("01_map_1to1", "M1: the player's own current map at 1:1, centred on a "
                           "5x5-block viewport clamped to the map's own bounds "
                           "(no connections/stitching -- that is M2); the red frame "
                           "marks the player's own block (3,2) -- the open floor "
                           "block, roughly centred on the 7-wide map -- NOT the PC "
                           "counter two rows up (D1: block = coord >> 1, one "
                           "halving, not two)")

    s.tap("SEL", settle=60)                                  # shell-wide toggle -> stretched
    s.shot("01b_stretched", "M1: SELECT stretches the same view to 240x160 -- the "
                             "shell's own scale toggle, not a map-specific key")
    s.tap("SEL", settle=60)                                  # back to 1:1
    s.shot("01c_1to1_again", "M1: SELECT again returns to 1:1")

    s.tap("L", settle=60)                                    # D6: L is ALSO the scale toggle here
    s.shot("01d_stretched_via_L", "M1 D6: L is the SAME scale toggle as SELECT on "
                                   "this screen (it does not pan) -- stretched again")
    s.tap("R", settle=60)                                    # R toggles it right back
    s.shot("01e_1to1_via_R", "M1 D6: R toggles it back to 1:1 -- L and R both drive "
                              "the one shared gb_scale_mode, same as SELECT, matching "
                              "the shell-wide 'L/R or SELECT' convention every other "
                              "GB screen uses")

    # VIRIDIAN_POKECENTER (Red.sav's real player map) is 7x4 blocks; the viewport
    # is 5x5 blocks. Vertically the WHOLE map already fits (height 4 <= VBH 5, so
    # vby is pinned at 0 the entire visit: no vertical pan is possible on THIS
    # map, not a bug -- Route 17's own vertical-clamp demo below covers that
    # axis). Horizontally the initial vbx is 1 (see the docstring's own D1
    # correction), one step of slack on EACH side: RIGHT once reaches the east
    # clamp (vbx=2, width 7 - VBW 5), a second RIGHT is a true no-op, then the
    # D-pad's LEFT (not L -- D6) walks it all the way back to the west clamp
    # (vbx=0), two presses, and a third LEFT is a true no-op there too.
    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.shot("02_panned_right_to_east_clamp", "M1: RIGHT once reaches the east clamp "
                                             "(vbx=1 -> 2, width 7 - the 5-block "
                                             "viewport = 2) -- the marker is no "
                                             "longer centred, now one block from "
                                             "the viewport's own LEFT edge (its "
                                             "fixed map block stayed put; the "
                                             "viewport panned right past it)")
    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.shot("03_east_clamp_no_op", "M1: a second RIGHT from the east clamp is a "
                                   "true no-op -- pixel-identical to the previous "
                                   "shot (vby also never moves off 0 this whole "
                                   "visit: height 4 <= the 5-block viewport)",
           allow_same=True)
    s.tap("LEFT", settle=gb_shots.SETTLE)
    s.shot("04_panned_left_via_dpad", "M1 D6: the D-PAD's own LEFT pans the "
                                       "viewport one block left (vbx=2 -> 1) -- L "
                                       "no longer does this (it is the SIZE "
                                       "toggle, shot 01d above)")
    s.tap("LEFT", settle=gb_shots.SETTLE)
    s.shot("05_west_clamp", "M1: LEFT again reaches the west clamp (vbx=0) -- the "
                             "marker is now near the viewport's own RIGHT edge "
                             "(three blocks from the left), the mirror image of "
                             "shot 02's east-clamp position")

    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # close -> back to the box grid
    s.shot("06_closed_back_to_grid", "M1: B closes the map screen -- back to the "
                                      "box grid. No save byte is ever written by "
                                      "this read-only screen; the shell's shared "
                                      "SIZE preference (gb_scale_mode, toggled "
                                      "twice above via L/R) still persists to "
                                      "config.cfg on close, exactly as on every "
                                      "other GB screen")

    return s


def run_m3_map_teleport(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """M3 (BACKLOG #91): the Gen-1 in-map teleport UI -- place-cursor mode, the
    confirm dialog, and the write path -- against Red's own current map, same
    Red-only fused image run_m1_map() uses.

    WHAT THE EMULATOR CAN AND CANNOT PROVE (read before editing this chain):
    app_can_edit() is TRUE in the PDNA_DELTA build (source/pdna_main.c: "there is
    no flashcart to gate on -- the save is our own flash chip, which is always
    writable"), so the PLACE row, the cursor, and the confirm dialog are all
    reachable and the field write itself (gbs_write_field, source/pdna_gbmap.c's
    gbmap_write_pos) genuinely lands in the resident image. What this build CANNOT
    reach is the "PLACED"/undo-offer/"RESTORED" messages: gb_persist()'s own
    PDNA_DELTA branch (source/pdna_gen12.c) always refuses ("Edits are in-session
    only in the emulator build") because a delta image has no SD card to persist
    to -- so shot 08 below is where this chain's proof stops. "PLACED" and
    "RESTORED" are HARDWARE-ONLY (HW-QUEUE), not faked here.

    Nav: identical to run_m1_map() up to the Map row (boot picker -> standalone ->
    START -> DOWN x16 -> A)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "m3_teleport_")
    print("== M3: Gen-1 in-map teleport (place cursor, confirm, write) ==")

    boot_to_gb_session(s, rom, which="red")
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box grid -> nav menu
    s.press_n("DOWN", 16)                                     # Party -> ... -> Map (index 16)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                    # Map -> pdna_gbmap_gen1()
    s.shot("01_map_view", "M3: the Map screen opens with the PLACE row now lit in "
                           "the right-side legend (can_edit is true in this build) "
                           "-- the red frame is the player's own current block "
                           "(3,2), same starting state as run_m1_map()'s own shot "
                           "01", claim=["PLACE", "BACK", "SIZE"])

    s.tap("A", settle=gb_shots.SETTLE)                        # A: enter place mode
    s.shot("02_placing_cursor_on_player", "M3: A enters place-cursor mode -- the "
                                           "green cursor frame is drawn on the SAME "
                                           "block as the player's own (red) marker, "
                                           "since the cursor always starts there, "
                                           "and drawn OVER it -- only the green "
                                           "frame is actually visible on this "
                                           "frame, not a second overlapping one "
                                           "(map-gen1 review D6); the legend is "
                                           "SWAPPED to PLACE/BACK only (D7: SIZE is "
                                           "inert while placing)",
           claim=["PLACE", "BACK"])

    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.shot("03_cursor_moved_right", "M3: the D-PAD moves the CURSOR now, not the "
                                     "viewport -- the green frame is one block "
                                     "right of the still-stationary red player "
                                     "marker")

    s.tap("B", settle=gb_shots.SETTLE)                        # B: cancel placing
    s.shot("04_cancelled_back_to_view", "M3: B cancels place mode without writing "
                                         "anything -- back to the plain view, "
                                         "pixel-identical in substance to shot 01 "
                                         "(only the red player marker remains)")

    s.tap("A", settle=gb_shots.SETTLE)                        # re-enter place mode
    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("05_cursor_two_blocks_away", "M3: re-entered place mode (cursor resets "
                                         "to the player's own block again) and "
                                         "moved RIGHT then DOWN -- the green "
                                         "cursor is now two blocks from the red "
                                         "player marker, a real, different, "
                                         "in-bounds destination")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # A: freeze + close the shell
    s.shot("06_confirm_dialog", "M3: A on a real (non-no-op) cursor position "
                                 "CLOSES the GB-screen shell first (pdna_gbtrainer."
                                 "c's own established shape for this codebase's "
                                 "shell -- see the KEY_A handler's own comment in "
                                 "source/pdna_gbmap.c) and only THEN shows the "
                                 "confirm dialog in plain Mode-3 UI, naming the "
                                 "exact destination block",
           claim=["PLACE CHARACTER HERE?", "Block (4,3)", "A = yes", "B = no"])

    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # confirm: write + persist
    s.shot("07_emulator_persist_refusal", "M3: A confirms -- gbmap_write_pos() "
                                           "derives+validates "
                                           "wCurrentTileBlockMapViewPointer "
                                           "(map-gen1 review D1) then writes it "
                                           "plus wYCoord/wXCoord/wYBlockCoord/"
                                           "wXBlockCoord into the resident image "
                                           "(gbs_write_field, verified/reparsed "
                                           "the same way every other GB field "
                                           "edit in this codebase is) and THEN "
                                           "gb_persist() is called -- its own "
                                           "PDNA_DELTA branch refuses to persist "
                                           "(no SD card in this build) and says "
                                           "so plainly; on real hardware this "
                                           "step instead shows PLACED and offers "
                                           "an immediate undo (HARDWARE-ONLY, not "
                                           "reachable here)",
           claim=["Edits are in-session only", "in the emulator build."])

    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # dismiss -> back to the box grid
    s.shot("08_back_to_box_grid", "M3: dismissing the refusal returns to the box "
                                   "grid -- pdna_gbmap_gen1() has already "
                                   "returned, same landing spot run_m1_map()'s "
                                   "own shot 06 (B-close) reaches -- the chain's "
                                   "own end state; re-opening Map would show the "
                                   "MOVED player position (map-gen1 review D6): "
                                   "gb_persist()'s own PDNA_DELTA branch "
                                   "re-baselines g_ed->pristine to the post-edit "
                                   "image BEFORE returning false (pdna_gen12.c), "
                                   "so the refusal is 'no card to write to', not "
                                   "'this edit is void' -- this build has no card "
                                   "at all, but the session's own resident image "
                                   "already carries the moved position")

    return s


def run_m1_map_vclamp(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """M1 (BACKLOG #91) vertical-clamp demo: VIRIDIAN_POKECENTER (Red.sav's real
    starting map) is only 4 blocks tall, shorter than the 5-block viewport, so
    run_m1_map() above can never move vby off 0 -- it has no vertical clamp to
    show. `rom` must be a Red-only fused image built from a save WARPED onto
    Route 17 (map id 28, 10x72 blocks -- tests/host_gbsurgery_tool.c's own
    `--op warp 28 8 68`, landing on block (4, 34): vby's own clamp range is
    [0, 72-5=67], and block 34 sits far enough from BOTH ends that a few UP/DOWN
    presses reach each one without an absurd number of taps.

    Nav: identical to run_m1_map() (same nav menu index, same screen) -- this is
    the SAME pdna_gbmap_gen1() reached from a differently-positioned save."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "m1_map_vclamp_")
    print("== M1: vertical clamp on Route 17 (warped save) ==")

    boot_to_gb_session(s, rom, which="red")
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 16)
    s.tap("A", settle=GB_ART_COLD_SETTLE)
    s.shot("01_route17_mid", "M1 vclamp: Route 17 (10x72 blocks), warped to block "
                              "(4,34) -- vby starts at 32 (34 - 5//2), comfortably "
                              "clamped on neither end")

    s.press_n("UP", 32, settle=gb_shots.SETTLE)
    s.shot("02_north_clamp", "M1 vclamp: 32 UPs reach the north clamp (vby=0)")
    s.tap("UP", settle=gb_shots.SETTLE)
    s.shot("03_north_clamp_no_op", "M1 vclamp: one more UP is a true no-op at the "
                                    "north clamp -- pixel-identical to the previous "
                                    "shot", allow_same=True)

    s.press_n("DOWN", 67, settle=gb_shots.SETTLE)
    s.shot("04_south_clamp", "M1 vclamp: 67 DOWNs from the north clamp reach the "
                              "south clamp (vby=67, height 72 - the 5-block "
                              "viewport)")
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("05_south_clamp_no_op", "M1 vclamp: one more DOWN is a true no-op at "
                                    "the south clamp", allow_same=True)

    s.tap("B", settle=gb_shots.BIG_SETTLE)
    s.shot("06_closed", "M1 vclamp: B closes the map screen, same as run_m1_map()")

    return s


def run_m1_map_gen2(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """M1-G2 (BACKLOG #91, docs/GB-MAP-DESIGN-G2.md): Crystal's OWN current-map
    view, read-only, on the shared GB-screen shell -- the Gen-2 twin of
    run_m1_map() above. `rom` MUST be a Crystal-ONLY fused image (Crystal.gbc+
    Crystal.sav, tools/fuse_gb.py, one ROM per image -- must NOT also carry an
    Emerald.sav, or a bare A on the boot picker opens Emerald's own screen
    instead of skipping straight to the GB save's box grid).

    Nav: single-ROM image, so `gb_delta_pick_save()`'s `if (n == 1) return 0`
    skips the boot picker entirely -- ONE tap (A -> box grid, #279: no info page), not
    DOWN+A+A the way run_m1_map()'s combined multi-ROM image needs. From the
    box grid: START -> nav menu -> DOWN x16 (same PDNA_NAV_ITEMS index as
    Gen 1's Map row -- the list order does not change per generation) -> A ->
    pdna_gbmap_gen2().

    Crystal.sav's real player position (group 24 / number 4 = NEW_BARK_TOWN,
    y=6/x=13 -> block (6,3) via gbmap_block_of()'s coord>>1) sits on a 10x9-
    block map: vbx starts at clampi(6-5//2, 0, 10-5=5) = 4, vby at
    clampi(3-5//2, 0, 9-5=4) = 1 -- the marker lands exactly centred on open
    ground (design doc §2.4/§8's own cross-checked New Bark Town dump)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "m1_map_g2_")
    print("== M1-G2: Crystal's own current-map view (single-ROM -> Map) ==")

    s.run(700)
    # #279: single-ROM image lands on the grid by itself (no S1 info page, no tap)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box grid -> nav menu
    s.press_n("DOWN", 16)                                     # Party -> ... -> Map (index 16, same as Gen 1)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                    # Map -> pdna_gbmap_gen2()
    s.shot("01_map_1to1", "M1-G2: New Bark Town at 1:1, 10x9 blocks, a 5x5-block "
                           "viewport starting exactly centred (vbx=4, vby=1) -- "
                           "the red frame marks the player's own block (6,3); "
                           "the VRAM tile-id remap (design doc §6) must resolve "
                           "every cell here, not just the trap maps (Ice Path/"
                           "Celadon Mansion Roof) -- a blank grid anywhere in "
                           "this shot is the remap failing")

    s.tap("SEL", settle=60)                                  # shell-wide toggle -> stretched
    s.shot("01b_stretched", "M1-G2: SELECT stretches the same view to 240x160, "
                             "same shell-wide scale toggle every other GB screen "
                             "uses")
    s.tap("SEL", settle=60)                                  # back to 1:1
    s.shot("01c_1to1_again", "M1-G2: SELECT again returns to 1:1")

    # width 10 - VBW 5 = 5 (east clamp); vbx starts at 4, one step of slack east,
    # four steps west down to 0 (west clamp).
    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.shot("02_east_clamp", "M1-G2: RIGHT once reaches the east clamp (vbx=4 -> "
                             "5, width 10 - the 5-block viewport)")
    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.shot("03_east_clamp_no_op", "M1-G2: a second RIGHT from the east clamp is "
                                   "a true no-op -- pixel-identical to the "
                                   "previous shot", allow_same=True)

    s.press_n("LEFT", 5, settle=gb_shots.SETTLE)
    s.shot("04_west_clamp", "M1-G2: five LEFTs from the east clamp reach the "
                             "west clamp (vbx=0) -- the marker is now near the "
                             "viewport's own RIGHT edge, the mirror image of "
                             "shot 02's east-clamp position")
    s.tap("LEFT", settle=gb_shots.SETTLE)
    s.shot("05_west_clamp_no_op", "M1-G2: one more LEFT at the west clamp is a "
                                   "true no-op", allow_same=True)

    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # close -> back to the box grid
    s.shot("06_closed_back_to_grid", "M1-G2: B closes the map screen -- back to "
                                      "the box grid. No save byte is ever "
                                      "written by this read-only screen")

    return s


def run_m1_map_gen2_wrong_game(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """M1-G2 (BACKLOG #91) wrong-game refusal (design doc §10 risk 1): `rom`
    MUST be a fused image carrying Crystal.gbc but a GOLD save (Gold.sav paired
    to Crystal.gbc by fuse_gb.py's own generation-only pairing -- the exact
    real-world footgun this guard exists for: a user's SD card has one Gen-2
    ROM and it happens to be the wrong one for the loaded save). Nav identical
    to run_m1_map_gen2() up to the Map row; pdna_gbmap_gen2() must refuse with
    PDNA_GBMAP2_WRONG_GAME instead of drawing Crystal's own New Bark Town under
    a Gold save."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "m1_map_g2_wronggame_")
    print("== M1-G2: wrong-game refusal (Gold save, Crystal-only ROM) ==")

    s.run(700)                       # #279: lands on the grid by itself (no info page, no tap)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 16)
    s.tap("A", settle=GB_ART_COLD_SETTLE)
    s.shot("01_wrong_game_refusal", "M1-G2 design §10 risk 1: a Gold save with "
                                     "only a Crystal ROM registered -- the "
                                     "cartridge header title check refuses "
                                     "(PDNA_GBMAP2_WRONG_GAME) instead of "
                                     "drawing a plausible but WRONG map")
    return s


def run_m1_map_gen2_no_rom(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """M1-G2 (BACKLOG #91) no-ROM refusal: `rom` MUST be a fused image carrying
    a Gen-2 save (Gold.sav) with NO Gen-2 ROM fused at all -- app_gb_rom_path()/
    gb_rom_path_beside() both fail. The shell's own gbscr_open() catches this
    BEFORE pdna_gbmap2.c's own "Could not open the ROM." fallback path is ever
    reached (that fallback covers a narrower case gbscr_open() itself does not
    catch) -- the real caption on this build is gbscr_open()'s own
    PDNA_GBSCR_REASON_ORPHANED_ROM message, "this save's ROM is not fused"
    (confirmed by the actual screenshot, not assumed)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "m1_map_g2_norom_")
    print("== M1-G2: no-ROM refusal (Gold save, no Gen-2 ROM fused) ==")

    s.run(700)                       # #279: lands on the grid by itself (no info page, no tap)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 16)
    s.tap("A", settle=GB_ART_COLD_SETTLE)
    s.shot("01_no_rom_refusal", "M1-G2: no Gen-2 ROM fused at all -- a clean "
                                 "'this save's ROM is not fused' refusal "
                                 "(gbscr_open()'s own PDNA_GBSCR_REASON_"
                                 "ORPHANED_ROM, fired before pdna_gbmap2.c's "
                                 "own narrower fallback is ever reached), not "
                                 "a crash or hang")
    return s


def run_u5_pack(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """U5 (BACKLOG #67, docs/GB-GAME-SCREENS-DESIGN.md sec 1.4): Gold/Silver/
    Crystal's OWN Pack + PC store on the shared GB-screen shell -- the Gen-2
    sibling of run_u4_bag() above. `rom` must be a ONE-Gen-2-ROM fused image
    (tools/fuse_gb.py fed only Gold.gbc+Gold.sav, or only Crystal.gbc+
    Crystal.sav -- BACKLOG #98's known harness gap, same reasoning as U4's own
    single-ROM requirement). A single-ROM image skips the boot picker entirely
    (gb_delta_pick_save()'s own `if (n == 1) return 0`, same as run_d7_gold()'s
    own doc comment) -- one A tap reaches the box grid directly (#279: no info page).

    Nav: A -> box grid (#279: no info page, rom_gbsprite cold scan) -> START -> nav menu
    -> DOWN x7 (Party->Bank->Daycare->Trainer->Clock fix->Mirage->Pokedex->Bag,
    PDNA_NAV_ITEMS index 7, SAME row order as Gen 1 -- nav_avail's GB_TABLE has
    one row per NV_* id with a per-generation COLUMN) -> A -> pdna_gbpack_gen2_
    screen() (gbscr_open()'s OWN cold rom_gbui scan)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"u5_{which}_")
    print(f"== U5: {which}'s own Pack (single-ROM image -> standalone -> Pack) ==")

    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 7)                                     # Party -> ... -> Bag (index 7)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Bag -> pdna_gbpack_gen2_screen()
    s.shot("01_items", f"U5: {which}'s OWN Pack, ITEMS pocket, 1:1 -- the "
                        "pocket picture (PackGFX/pack_m, real ROM art) and the "
                        "static header/nameplate art (the derived PACKMENU "
                        "block), 5 item rows visible, cursor on row 1")

    s.tap("SEL", settle=60)
    s.shot("01b_stretched", "U5: SELECT stretches the same list to 240x160")
    s.tap("SEL", settle=60)
    s.shot("01c_1to1_again", "U5: SELECT again returns to 1:1")

    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Items -> Balls
    s.shot("02_balls", "U5: RIGHT cycles to the BALLS pocket -- a different "
                        "nameplate label + pocket picture slice (PackGFX ROM "
                        "index 3), the CANCEL row (real-cartridge fact, corrects "
                        "an earlier design-doc guess that Gen 2 has no CANCEL "
                        "row -- confirmed live on this exact save)")
    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Balls -> Key items
    s.shot("03_key_items", "U5: RIGHT again -> KEY ITEMS -- no quantity column "
                            "at all (blank, not '-'), matching the real "
                            "cartridge's own posture for this pocket")
    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Key items -> TM/HM
    s.shot("04_tmhm", "U5: RIGHT again -> TM/HM -- the two-digit TM/HM number "
                       "prefix (cols 5-6, before the cursor) plus a 'TMnn'-style "
                       "fallback name (no move-name table this slice) and its "
                       "own count column. Also D4's own fix: every row here is "
                       "an OWNED TM/HM (this save's TM09 is missing and never "
                       "appears as a blank or zero-count row, matching the "
                       "real cartridge's TMHM_DisplayPocketItems)")

    # D3 fix (review-opus ac9ffc0): the cursor walks all ROWS_VISIBLE (5) rows
    # before `top` moves, matching the real cartridge's own gold_down4 ground
    # truth -- NOT Gen 1's bag-style pin-at-slot-2 the earlier draft borrowed.
    # This save's TM/HM pocket has 56 owned entries (D4), plenty to scroll.
    s.press_n("DOWN", 4, settle=gb_shots.SETTLE)
    s.shot("04b_scrolled_unpinned", "U5 D3 fix: TM/HM, 4 DOWNs from the top -- "
                                     "the cursor is on the FIFTH visible row "
                                     "and the list has NOT scrolled yet (`top` "
                                     "is still 0) -- the cursor walks the whole "
                                     "visible page before scrolling starts")
    s.press_n("DOWN", 4, settle=gb_shots.SETTLE)
    s.shot("04c_scrolled_further", "U5 D3: four MORE DOWNs (8 total) -- now "
                                    "the list has actually scrolled, `top` "
                                    "tracking the cursor one row at a time")
    s.press_n("UP", 8, settle=gb_shots.SETTLE)              # back to row 0 for the rest of the flow

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (TM/HM: ADD ITEM/PC STORE/CANCEL --
                                                              # BACKLOG #195 F2 now offers ADD ITEM here too,
                                                              # gbb_tmhm_set() path -- PC STORE moved from
                                                              # csel 0 to csel 1, ONE DOWN needed)
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # ADD ITEM -> PC STORE
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # toggle -> PC store
    s.shot("05_pc_store", "U5: START > PC STORE toggles to the PC item store -- "
                           "the nameplate label under the picture still reads "
                           "'Items' (reused, not overridden; not independently "
                           "pixel-dumped this slice, see pdna_gbpack.h). D6 "
                           "fix: the description box is the ONE thing that "
                           "changes, now printing 'PC ITEM STORE' -- the "
                           "earlier draft's empty box left the store visually "
                           "identical to the Items pocket")

    # BACKLOG #195 F2: ADD ITEM now opens the real item picker (pick_item(),
    # restricted to GBIN_GEN2 with the Gen-2 ceiling, PRE-FILTERED to the pocket
    # the menu was opened from via pick_item_set_gen1_2_cat) instead of a raw
    # numeric "ITEM ID" prompt. The old "WRONG POCKET / no per-item pocket table
    # yet" refusal (D9's own fix, ac9ffc0) is GONE -- gbb_pocket_of() IS that
    # table now: a picked id auto-routes to its OWN real pocket (Items/Balls/
    # Key/TM-HM), the Gen-3 bag's own shape (pdna_bag.c:445-452), regardless of
    # which pocket's ADD ITEM menu opened it. The PC store keeps its D7 rule
    # unchanged (any id, no routing -- it is its own undifferentiated list).
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (PC store, csel=0=ADD ITEM)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> pick_item() opens (cat=All: the PC
                                                              # store is not one of the 4 real-pocket categories)
    s.shot("05b_pc_store_add_item", "U5 (BACKLOG #195): ADD ITEM from the PC "
                                     "ITEM STORE opens the SAME real picker as "
                                     "every other pocket -- names + header "
                                     "'ITEM [All]' (the store has no category "
                                     "of its own, D7's 'any id' rule) -- this "
                                     "save's PC store happens to already be "
                                     "full, so this demo cancels out (B, the "
                                     "picker's own cancel key) rather than "
                                     "complete an insert that would correctly "
                                     "show BAG FULL, a different, expected "
                                     "refusal")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # pick_item()'s own cancel (B, NOT SELECT --
                                                              # SELECT opens the picker's search box instead)
                                                              # -> back in the SAME open PACK MENU (csel still 0)

    s.press_n("DOWN", 3, settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # toggle back -> Pack (still on TM/HM's own cyc)
    s.shot("06_back_to_pack", "U5: START > PACK toggles back")

    s.tap("LEFT", settle=gb_shots.BIG_SETTLE)               # TM/HM -> Key items
    s.tap("LEFT", settle=gb_shots.BIG_SETTLE)               # Key items -> Balls
    s.tap("LEFT", settle=gb_shots.BIG_SETTLE)               # Balls -> Items
    s.shot("07_left_wraps_to_items", "U5: LEFT cycles the other way -- back on "
                                      "ITEMS")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the selected item -> qty editor
    s.shot("08_qty_editor", "U5: A on the selected item opens the existing "
                             "numeric editor (num_entry, 1-99) -- the SAME "
                             "pop-up every other GB screen's own field edits "
                             "use")
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # osk_search: SELECT cancels

    # BACKLOG #195 F2, "pick a Ball from the Items pocket -> lands in Balls":
    # ADD ITEM from the ITEMS pocket, pick a Poke Ball through the picker's OWN
    # category filter, and prove the auto-route by switching to the BALLS
    # pocket afterward. gold_ball_ids (tests/host_gbbag_test.c's own corpus
    # constants) confirms Gold.sav's REAL Balls pocket already holds MASTER/
    # ULTRA/GREAT/POKE (0x01/0x02/0x04/0x05) -- HEAVY_BALL (0x9D) is NOT among
    # them, so adding it is a fresh insert (a new row appears), not a merge
    # into an existing stack (which BALLS support just as well, but a fresh
    # row is the clearer demo).
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (Items, csel=0=ADD ITEM)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> picker opens, PRE-FILTERED to Items
                                                              # (pick_item_set_gen1_2_cat(GBB_POCKET_ITEMS))
    s.shot("09_picker_open_items", "U5 (BACKLOG #195): ADD ITEM from the Items "
                                    "pocket opens the picker PRE-FILTERED to "
                                    "'Items' -- real Gen-2 names + a category "
                                    "header ('ITEM [Items]'), same picker U4's "
                                    "Gen-1 screen uses, Gen-2's own pocket set")
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> the picker's OWN category menu
    s.shot("09b_category_menu", "U4: the restricted picker's category menu -- "
                                 "Gen 2 offers All/Items/Poke Balls/Key items/"
                                 "TM-HM (gbb_pocket_of()'s full Gen-2 pocket "
                                 "set, unlike Gen 1's ITEMS-vs-TM/HM-only), "
                                 "cursor on 'Items' (the pre-filter this menu "
                                 "opened with, not 'All')")
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # Items -> Poke Balls
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick Poke Balls -> re-filters, closes menu
    s.shot("09c_balls_filtered", "U5: picking 'Poke Balls' re-filters the SAME "
                                  "picker through gbb_pocket_of() -- header "
                                  "'ITEM [Poke Balls]', only the 12 real Ball "
                                  "ids, cursor back on MASTER BALL (row 0)")
    s.press_n("DOWN", 4, settle=gb_shots.SETTLE)            # MASTER/ULTRA/GREAT/POKE (4 rows) -> HEAVY BALL
    s.shot("09d_heavyball_selected", "U5: 4 DOWNs from MASTER BALL (the 4 "
                                      "ordinary balls, id order 0x01/0x02/0x04/"
                                      "0x05) lands on HEAVY BALL (0x9D, the "
                                      "next Ball id) -- a real name, not '#157'")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick HEAVY BALL -> QUANTITY prompt
    s.shot("09e_add_item_qty", "U5: picking a Ball goes straight to the SAME "
                                "quantity prompt as any other Gen-2 item -- "
                                "BACKLOG #195 F2 only replaced the id half of "
                                "ADD ITEM")
    s.tap("B", settle=gb_shots.SETTLE)                      # clear the seeded "1"
    s.tap("A", settle=gb_shots.SETTLE)                      # col0 IS '1' -- type it directly -> field "1"
    s.press_n("RIGHT", 9, settle=gb_shots.SETTLE)           # col0 -> col9 '0'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '0' -> field "10"
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=10 -> gbb_pocket_of(game,0x9D)=BALLS
                                                              # != ITEMS (the pocket this menu opened from) ->
                                                              # AUTO-ROUTES: gbb_insert(game,bag,BALLS,0x9D,10),
                                                              # a fresh slot, GBB_OK -- Review D4: a SUCCESSFUL
                                                              # route to a DIFFERENT pocket than this menu opened
                                                              # from now tells the player where it landed
                                                              # (pdna_bag.c:447-451's own "RIGHT POCKET" shape).
    s.shot("09f_right_pocket", "U5 (Review D4, UX parity with pdna_bag.c's "
                                "own Gen-3 routing feedback): 'RIGHT POCKET / "
                                "Put in BALLS.' -- the SAME pocket name "
                                "pocket_name_of() gives every other message "
                                "in this file, now also telling the player "
                                "the item did NOT stay in Items")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss the msg_wait -- back on the Items list
    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Items -> Balls: does the row actually land there?
    s.shot("10_lands_in_balls", "U5 (BACKLOG #195, 'a Ball picked from the "
                                 "Items pocket -> lands in Balls'): switching "
                                 "to the BALLS pocket shows HEAVY BALL x10 -- a "
                                 "brand-new row, auto-routed by gbb_pocket_of() "
                                 "even though ADD ITEM was opened from Items, "
                                 "not Balls")

    # Review D2: pokegold's OWN data/items/attributes.asm differs from
    # pokecrystal's at exactly four ids (CLEAR_BELL/GS_BALL/BLUE_CARD/
    # EGG_TICKET, 0x46/0x73/0x74/0x81) -- KEY_ITEM on Crystal, an unused
    # pocket-ITEM placeholder on Gold/Silver. The picker's "Key items"
    # category must show a DIFFERENT count/list on Gold vs Crystal even
    # though this is the exact same code path -- proving `game`, not merely
    # `gen`, now threads all the way to gbb_pocket_of(). Cancelled out (B,
    # not committed) so it leaves no pending edit for the demos after it.
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (Balls, fresh open, csel=0=ADD ITEM)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> picker, pre-filtered to Poke Balls
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> category menu (cursor on Poke Balls)
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # Poke Balls -> Key items
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick Key items -> re-filters, closes menu
    s.shot("10b_key_items_filtered", f"U5 (Review D2): the picker's 'Key "
                                      f"items' category on {which} -- Gold "
                                      f"shows 18 (no CLEAR BELL/GS BALL/"
                                      f"BLUE CARD/EGG TICKET), Crystal shows "
                                      f"22 (all four present, interleaved in "
                                      f"id order between MYSTERY EGG/SILVER "
                                      f"WING and after SILVER WING) -- the "
                                      f"SAME code path, a real per-game "
                                      f"difference (pokegold's own "
                                      f"attributes.asm, not pokecrystal's)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # cancel the picker -- no id picked, no pending edit
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # close PACK MENU -> back to the Balls list

    # SWAP: arm row 0 (MASTER BALL), move down, confirm the destination --
    # unrelated to pick_item()/gbb_pocket_of() at all, same mechanism U4's own
    # Gen-1 SWAP demo already proved; reused here over the Balls pocket instead.
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (Balls, fresh open, csel=0=ADD ITEM)
    s.press_n("DOWN", 2, settle=gb_shots.SETTLE)            # ADD ITEM -> REMOVE -> SWAP
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # SWAP: arms row 0 (MASTER BALL), returns
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # cursor off the source row
    s.shot("11_swap_armed", "U5: SWAP arms row 0 (the mark stays lit there) "
                             "and returns to the list -- pick-source-then-"
                             "destination, same semantic as U4's own Gen-1 "
                             "SWAP")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the destination -> the actual swap
    s.shot("11b_swap_done", "U5: A on the destination performs the swap -- "
                             "both rows traded places, the mark is gone")
    # Undo the swap so this pocket's ORDINARY rows are back where the fixture
    # had them (only HEAVY BALL, appended at the tail, is a real pending edit
    # left for the commit-prompt demo below).
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 2, settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # re-arm SWAP on row 1 (cursor's current row)
    s.tap("UP", settle=gb_shots.SETTLE)                     # cursor -> row 0 (the OTHER half of the pair)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # swap back -> pristine order restored
    s.shot("11c_swap_undone", "U5: swapping row 0/row 1 back -- MASTER BALL/"
                               "ULTRA BALL are back in their original order; "
                               "HEAVY BALL (this pocket's real pending edit) "
                               "is untouched by any of the swap demos")

    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B with a real pending edit (HEAVY BALL, still
                                                              # unsaved) -> commit prompt
    s.shot("12_commit_prompt", "U5: B with a real pending edit -> 'Save pack "
                                "changes?' (app_confirm), the same dialog "
                                "every other GB screen's own commit uses")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # decline
    s.shot("13_declined", "U5: declining discards the edit -- gbb_write never "
                           "ran, back at the box grid")

    # Review D1 (HIGH, data loss): gbpack_add_routed()'s TM/HM branch used to
    # call num_entry() (cancel-blind: returns `cur`, pdna_trainer.c:47-50)
    # SEEDED AT 0, so opening COUNT on an ALREADY-OWNED TM showed 0 instead of
    # its real count, and cancelling (osk_search's own cancel key -- SELECT,
    # not B; B is backspace, same contract every other prompt in this file
    # already uses) SET that TM's count to 0 instead of leaving it alone --
    # an owned TM01 x1 became x0 on a cancelled ADD. Fixed to num_entry_opt()
    # seeded at the REAL current count (gbb_tmhm_get()), cancel now returns
    # false (nothing attempted, same contract gbpack_add_to_pc() already
    # gives its own caller). No shot of this whole path existed before this
    # fix -- added here (rule-17 blocker: a gesture/data-path with no shot
    # proof is not merged).
    s.run(250)                                              # re-entry idle (see U4's own D-reentry note)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 7, settle=gb_shots.SETTLE)            # Party -> ... -> Bag (index 7)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Bag -> pdna_gbpack_gen2_screen()
    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Items -> Balls
    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Balls -> Key items
    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Key items -> TM/HM
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (TM/HM: ADD ITEM/PC STORE/CANCEL)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> picker, pre-filtered to TM-HM
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick TM01 (id 0xBF, the lowest TM/HM id --
                                                              # row 0 of the TM-HM filtered list) -> COUNT
    s.shot("14_count_seeded_current", "U5 (Review D1 fix): the COUNT prompt "
                                       "for an ALREADY-OWNED TM (TM01, this "
                                       "save's own real count) now seeds at "
                                       "'1' -- its ACTUAL current count, not "
                                       "the old bug's hardcoded 0")
    s.tap("B", settle=gb_shots.SETTLE)                      # backspace the seeded '1'
    s.press_n("RIGHT", 2, settle=gb_shots.SETTLE)           # digit row: col0 -> col2 '3'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '3' -> field "3"
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm -> gbb_tmhm_set(tm01,3): GBB_OK
    s.shot("15_tm01_count3", "U5 (Review D1, 'TM01 COUNT 3 -> row'): TM01's "
                              "row now reads x3 -- a real, persisted-if-"
                              "confirmed change")
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU again, fresh open
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> picker, TM-HM filtered
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick TM01 again -> COUNT seeded at the NEW
                                                              # current count (3, not the old bug's 0)
    s.shot("16_count_reseeded_3", "U5 (Review D1): re-opening COUNT on TM01 "
                                   "now seeds '3' -- the count this ADD "
                                   "chain itself just wrote, not 0")
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # osk_search's own CANCEL key (SELECT, not
                                                              # B -- B is backspace) -- num_entry_opt()
                                                              # returns false, gbpack_add_routed() returns
                                                              # false, the caller's own `continue` re-opens
                                                              # PACK MENU with NOTHING written
    s.shot("17_cancel_no_zero", "U5 (Review D1, 'B at COUNT -> count "
                                 "unchanged'): cancelling lands back on PACK "
                                 "MENU (its own `continue`, not the list) "
                                 "with NO gbb_tmhm_set() call made at all -- "
                                 "the fix's whole point, provable only by "
                                 "the NEXT shot still reading x3, not x0")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # close PACK MENU -> back to the list
    s.shot("18_still_x3", "U5 (Review D1): TM01 still reads x3 -- the cancel "
                           "above did NOT zero it (the old bug's exact "
                           "failure mode)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # leave -- the x1->x3 ADD is still a real
                                                              # pending edit (the cancel demo added nothing
                                                              # further) -> commit prompt
    s.shot("19_commit_prompt", "U5: leaving with TM01's real x1->x3 edit "
                                "still pending -> the same 'Save pack "
                                "changes?' dialog every other commit uses")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # decline -- fixture stays pristine
    s.shot("20_declined", "U5: declining discards TM01's edit -- gbb_write "
                           "never ran, fixture pristine for a later run")

    return s


def run_b89_hof(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #89 (BACKLOG #202 F1 recaption): the Gen-1/2 Hall of Fame screen
    (source/pdna_gbhof.c) -- the same single-ROM-image / nav-menu-DOWN shape
    run_b90_fly() above uses. `rom` must be a ONE-ROM fused image (Red-only for
    `which == "red"`, Crystal-only for `which == "crystal"`, same BACKLOG #98
    harness-gap reasoning as U4/U5/b90's own images).

    #202 F1: pdna_gbhof() now opens the SAME gbscr card shell the trainer card
    uses (GBSCR_NEED_TEXTBOX only) and keeps it open across list<->detail<->the
    START menu; every gbscr_open() call pays the PDNA_DELTA leg's whole-ROM UI
    locator cost again (~3,240 frames measured, review of b194/f93cf5a) --
    EVERY settle below that follows a shell (re)open (the very first Records
    entry, and returning to the list after CLEAR ALL/SET COUNT close+reopen the
    shell around their own full-screen editors) now rides out GB_ART_COLD_SETTLE,
    not BIG_SETTLE; list<->detail<->menu transitions stay on the shell that is
    ALREADY open (no rescan) and only need BIG_SETTLE.

    Nav: A -> box grid (#279: no info page) -> START -> nav menu -> DOWN x12 (Party=0, Bank=1,
    Daycare=2, Trainer=3, Clock fix=4, Mirage=5, Pokedex=6, Bag=7, Flags&counters=8,
    Bases=9, Blocks=10, Tickets=11, Records=12 -- PDNA_NAV_ITEMS order,
    source/pdna_layout.h) -> A -> pdna_gbhof()."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b89_{which}_")
    print(f"== BACKLOG #89: {which}'s own Hall of Fame (single-ROM image -> "
          "standalone -> Records) ==")

    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 12)                                    # Party -> ... -> Records (index 12)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Records -> pdna_gbhof() -> FIRST gbscr_open()
    s.shot("01_list", f"BACKLOG #202 F1: {which}'s own Hall of Fame CARD -- the "
                       "'N teams (life C)' header drawn with the ROM's own font "
                       "inside the shared gbscr text-box frame, one 'N: ...' row "
                       "per recorded team newest-first (F2: no '#', no '>' -- the "
                       "row cursor is the gbscr_cell_rect() highlight on row 1)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # row 1 -> the team detail (SAME shell, no reopen)
    # D6: this corpus save's own HoF teams carry no shiny DV quad and no custom
    # nickname (real, unedited saves) -- do not claim either in THIS shot's own
    # caption. Both are proven on dedicated poked-.sav shots kept alongside this
    # set (b89_{red,crystal}_08_nick.png, b89_crystal_09_shiny.png), not implied here.
    # BACKLOG #214 item 2: this row list is gbscr_text() -- the ROM's OWN composited
    # tile font (source/pdna_gbhof.c's hof_card_paint_detail(), no source/ui_font.c/
    # tonc sys8 involved) -- previously OUT of gb_claims.py's scope (b184 reverted
    # this exact class of retrofit, see that lane's own comment on run_b89_hof/03_menu
    # below). gb_claims.check_gb()/find_gb() now read the ROM this Session actually
    # booted from (Session._gb_rom_file(), extracted from THIS image's own fused GB
    # ROM entry) -- the first species row of each corpus save's real HoF team,
    # per-game since Red's and Crystal's own corpus teams differ.
    claim_species = {"red": "MEWTWO", "crystal": "TYPHLOSION"}[which]
    s.shot("02_detail", "BACKLOG #202 F1: the team detail CARD -- 6 mon rows "
                        "(species/level), still inside the SAME open shell (no "
                        "reopen, no rescan cost) -- OT id is dropped from the "
                        "card view (no room for a 3rd row/mon); it stays on the "
                        "plain fallback page",
                        claim_gb=claim_species)
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # detail -> back to the list (same shell)

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # the Hall of Fame's own START menu (same shell)
    # BACKLOG #184 retrofit attempted here and REVERTED: this screen is
    # hof_card_paint_menu() (source/pdna_gbhof.c), which paints INSIDE the gbscr
    # card shell via gbscr_text()/hof_card_text_fit() -- the ROM's OWN composited
    # tile font, not source/ui_font.c's ui_font_bits nor tonc's sys8Font. A live
    # mGBA run with claim=["CLEAR ALL","SET COUNT","ADD TEAM","DELETE TEAM"]
    # proved this empirically ([CLAIM FAILED] on all four against the real frame,
    # even though the text is plainly visible by eye -- see the b184 executor
    # report's pixel dump). This is exactly the GB-shell case the brief scoped
    # out of gb_claims.py's v1 (tools/gb_oracle/oracle.py's tile->glyph map needs
    # a live GB core session this Session/PNG-only harness does not have) -- no
    # claim= here until that lands.
    s.shot("03_menu", "BACKLOG #202 F1: START -> the shell's own in-frame menu -- "
                       "CLEAR ALL / SET COUNT / ADD TEAM / DELETE TEAM (BACKLOG "
                       "#194 F3's two newer rows, recaptioned here -- this frame "
                       "used to show only the first two)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # CLEAR ALL -> app_confirm (A2: shell stays
                                                              # OPEN underneath; this full-screen ui_*
                                                              # dialog just paints over it for now)
    s.shot("04_clear_confirm", "BACKLOG #89: CLEAR ALL -> the real consequence -- "
                                "\"The PC's HALL OF FAME option disappears until "
                                "you win again.\" (app_confirm, full-screen -- A2: "
                                "the shell is never closed for this, only "
                                "repainted once the whole CLEAR ALL flow returns)")
    # gbh_clear() on Gen 1 chunks its write over up to 50 gen1_write_outside_sum
    # calls (one per team slot), each re-opening/re-parsing the whole 32 KiB image
    # to verify -- genuinely more CPU work than any other GB screen's single-field
    # commit, so BIG_SETTLE (40 frames, sized for a plain screen repaint) is not
    # enough to reach gb_persist()'s own dialog: caught live by gb_shots.Session's
    # identical-frame guard (the confirm's "yes" tap registers, but the shot landed
    # before gbh_clear()+gb_persist() had finished computing and drawing anything
    # new -- computation still running, not a stuck input). A longer settle first.
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # confirm -> gbh_clear() -> gb_persist()
    s.run(600)                                               # ride out the 50-chunk clear
    s.shot("05_emu_refusal", "BACKLOG #89: gb_persist()'s PDNA_DELTA branch "
                             "refuses ('Edits are in-session only in the emulator "
                             "build.') -- same #62 D2/D5 branch every other GB "
                             "screen's own commit hits here; the real write path is "
                             "proved by the retail gate's hofclear/hofcount cases "
                             "(tools/gb_retail_gate.py), not this shot. The edit "
                             "already landed in-session, shown by the NEXT shot.")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss gb_persist's dialog -- A2: no
                                                              # reopen, hof_card_menu_key just repaints
                                                              # the SAME still-open shell
    s.shot("06_empty", "BACKLOG #202 A2: after CLEAR ALL -- back on the SAME "
                        "card (never closed) -- '0 teams (life 0)', "
                        "'No teams yet.'")

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # menu again, on the now-empty list (same shell)
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # SET COUNT row
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # -> the stepper, starts at 0 (never confirmed,
                                                              # so gb_persist()'s own dialog never fires here)
    s.press_n("UP", 3, settle=gb_shots.SETTLE)              # 0 -> 3, or pinned at 0 under D1 (see below)
    # D1: on Gen 1 the cap is the teams present, not a flat 255 -- and this screen
    # was reached AFTER CLEAR ALL (06_empty), so present=0 here and every UP press
    # clamps right back to 0. Gen 2's own cap stays a flat 200 regardless (its own
    # viewer re-derives the count from the slots).
    if which == "crystal":
        cap, want = 200, 3
    else:
        cap, want = 0, 0
    s.shot("07_setcount", f"BACKLOG #89: SET COUNT's stepper after CLEAR ALL -- "
                          f"the ceiling is gbh_slots_in_blob() (0 here, this list "
                          f"is empty, R1), not a flat 255, so 3 UP presses land at "
                          f"{want} ('Lifetime wins: {want} / {cap}')")

    return s


def run_b194_hof(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #202 F4: the deeper card-shell flows run_b89_hof() above does not
    reach -- a cold list open, the detail card, the in-frame START menu, an
    EDIT of one mon's level through to the emulator's in-session refusal, ADD
    TEAM through to the SAME refusal, and DELETE TEAM. `rom` MUST be the SAME
    kind of ONE-ROM fused image run_b89_hof() takes.

    A1 (review re-verify): Gen 2's detail rows now draw each mon's real 16x16
    ROM menu icon at the row's left (hof_card_icon_refresh/_blit,
    source/pdna_gbhof.c) -- 02_detail's own caption says so for `which ==
    "crystal"`; Gen 1 stays text-only (a real gap, not a placeholder-icon
    omission -- that function's own comment has the honest reason).

    A2 (review re-verify): the shell now stays OPEN across EVERY full-screen
    sub-editor too (EDIT MON's fields, CLEAR ALL/SET COUNT/ADD TEAM/DELETE
    TEAM) -- there is only ONE gbscr_open() in this whole run, the FIRST
    Records entry below; every dismiss/return shot's own caption says
    "back on the card" now, not "reopened" (the old two-open-per-edit
    behaviour this runner's captions used to describe was itself A2's bug).
    GB_ART_COLD_SETTLE is still used ONLY after that one open."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b194_{which}_")
    print(f"== BACKLOG #202 F4: {which}'s own HoF card -- cold open, menu, edit, "
          "add, delete ==")

    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box grid -> nav menu
    s.press_n("DOWN", 12)                                     # Party -> ... -> Records
    s.tap("A", settle=GB_ART_COLD_SETTLE)                     # Records -> pdna_gbhof() -> FIRST gbscr_open()
    s.shot("01_cold_list", f"BACKLOG #202 F4: {which}'s HoF card, COLD open -- "
                            f"the FIRST gbscr_open() this session, ridden out with "
                            f"GB_ART_COLD_SETTLE ({GB_ART_COLD_SETTLE} frames) -- "
                            f"the same PDNA_DELTA whole-ROM locator cost the "
                            f"b194/f93cf5a review measured at ~3,240 frames")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # row 1 -> detail (same open shell)
    if which == "crystal":
        cap02 = ("BACKLOG #202 A1 (BACKLOG #198 item 12 recaption): the detail "
                 "card, same open shell -- each mon's real 16x16 Gen-2 ROM menu "
                 "icon at the row's left (hof_card_icon_refresh/_blit), text "
                 "shifted to column 3 to leave room -- this is the ROM's own "
                 "ICON CLASS per species, not a per-species portrait: rows 4-6 "
                 "(TYRANITAR/DELIBIRD/GRANBULL) render the SAME icon by design "
                 "(Gen 2 has ~36 shared icon classes, pokecrystal's own data/"
                 "pokemon/menu_icons.asm, reference only -- those three all map "
                 "to ICON_MONSTER; TYPHLOSION=ICON_FOX, NOCTOWL=ICON_BIRD, "
                 "MANTINE=ICON_FISH are each their own class), not a bug")
    else:
        cap02 = ("BACKLOG #202 F4: the detail card, same open shell -- Gen 1 "
                 "stays TEXT ONLY (a real gap: no per-species front-pic cache "
                 "yet, BACKLOG #196), not a placeholder icon")
    s.shot("02_detail", cap02)

    s.tap("B", settle=gb_shots.BIG_SETTLE)                    # detail -> list (same shell)
    s.tap("START", settle=gb_shots.BIG_SETTLE)                # list -> the in-frame START menu
    s.shot("03_menu_in_frame", "BACKLOG #202 F1: the START menu drawn INSIDE "
                                "the SAME card frame (no ui_clear() screen swap) "
                                "-- CLEAR ALL / SET COUNT / ADD TEAM / DELETE TEAM")

    # ---- EDIT a level, through to the emulator's own in-session refusal -----------
    s.tap("B", settle=gb_shots.BIG_SETTLE)                    # menu -> list
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # row 1 -> detail
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # mon 0 -> EDIT MON menu (full-screen,
                                                                # shell closed for this sub-editor -- F3)
    s.tap("DOWN", settle=gb_shots.SETTLE)                     # SPECIES -> LEVEL
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # -> the level stepper
    s.press_n("UP", 3, settle=gb_shots.SETTLE)                # +3 levels (clamped at 100 if already there)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # set -> back on EDIT MON, dirty=true
    s.shot("04_level_staged", "BACKLOG #202 F4: LEVEL stepper committed (+3, "
                               "clamped at 100) -- back on the EDIT MON menu, "
                               "staged, nothing written yet")
    # ---- D4 (b194 review): B on a DIRTY edit confirms before discarding ----------
    # hof_edit_mon_menu's own KEY_B handler (source/pdna_gbhof.c:288-298): with
    # dirty==true (the level stage above set it), B no longer discards silently --
    # it opens PDNA_GBHOF_DISCARD_TITLE ("Discard changes?" / "Your changes to
    # this mon will be lost.", A = discard, B = stay). BACKLOG #198 item 6: this
    # arm had no shot coverage anywhere in this runner -- the pre-existing flow
    # only ever reached DONE (committing), never B-with-dirty (discarding).
    s.tap("B", settle=gb_shots.BIG_SETTLE)                    # B, dirty=true -> the discard confirm
    s.shot("04b_discard_confirm", "BACKLOG #198 item 6 (D4, b194 review): B on "
                                   "the DIRTY LEVEL edit -- 'Discard changes?' / "
                                   "'Your changes to this mon will be lost.' "
                                   "(app_confirm, A = discard, B = stay)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                    # decline (stay) -> back on EDIT MON,
                                                                # the staged LEVEL change still intact
    s.shot("04c_discard_declined", "BACKLOG #198 item 6 (D4): B on the confirm "
                                    "(decline/stay) -- back on the EDIT MON menu "
                                    "with the staged +3 LEVEL change INTACT, "
                                    "nothing thrown away (the confirm's own "
                                    "`continue`, not a `return false`)")
    s.press_n("DOWN", 2, settle=gb_shots.SETTLE)              # LEVEL -> NICKNAME -> DONE
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # DONE (dirty) -> "Save changes to slot 1?"
    s.shot("05_edit_confirm", "BACKLOG #202 F4: DONE with a real staged change -- "
                               "'Save changes to slot 1?' (app_confirm)")
    # Two SEPARATE taps, same shape as run_b89_hof's own CLEAR ALL sequence:
    # (1) yes -> gbh_set_mon() -> gb_persist() -> the PDNA_DELTA refusal dialog
    # appears (own settle to let the write finish before the dialog draws);
    # (2) a SECOND tap dismisses THAT dialog -- A2: the shell was NEVER
    # closed for this edit, so there is nothing to reopen; the card is simply
    # repainted (hof_card_detail_key's own gbscr_mark_all_dirty()).
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # yes -> gb_persist()
    s.run(200)                                                 # ride out the single gen1_write_outside_sum
    s.shot("06_edit_refusal", "BACKLOG #202 F4: gb_persist()'s PDNA_DELTA "
                               "refusal, same #62 D2/D5 branch as every other "
                               "GB screen's own commit -- the edit already "
                               "landed in-session, shown by the NEXT shot")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # dismiss -- A2: the SAME still-open shell
                                                                # just repaints, no reopen at all
    s.shot("07_edit_back_on_card", "BACKLOG #202 A2: dismissed -- back on the "
                                    "DETAIL card (the SAME shell, never closed) "
                                    "showing the edit already landed in-session")

    # ---- ADD TEAM, through to the SAME in-session refusal --------------------------
    s.tap("B", settle=gb_shots.BIG_SETTLE)                    # detail -> list
    s.tap("START", settle=gb_shots.BIG_SETTLE)                # list -> menu
    s.press_n("DOWN", 2, settle=gb_shots.SETTLE)              # CLEAR ALL -> SET COUNT -> ADD TEAM
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # ADD TEAM -> the full-screen builder
                                                                # (shell closed -- F3)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # + ADD MON -> the species picker
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # pick the default species -> the level stepper
    s.tap("B", settle=gb_shots.BIG_SETTLE)                    # cancel (keep the Lv5 default)
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                  # cancel the nickname OSK (keep the default)
    s.shot("08_add_one_mon", "BACKLOG #202 F4: ADD TEAM's builder with 1 mon "
                              "staged (default species, Lv5, default nickname)")
    s.tap("DOWN", settle=gb_shots.SETTLE)                     # + ADD MON -> DONE
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # DONE -> "Add this 1-mon team?"
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # yes -> gbh_append_team -> gb_persist()
    s.run(200)
    s.shot("09_add_refusal", "BACKLOG #202 F4: ADD TEAM hits the SAME "
                              "in-session-only refusal as CLEAR ALL/EDIT")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # dismiss -- A2: no reopen, same shell
    # BACKLOG #198 item 13: the team count is this corpus save's own baseline +1
    # (ADD TEAM's real append), NOT a flat number -- Red's own Red.sav starts at
    # 9 teams (-> 10 after this add), Crystal's own Crystal.sav starts at 6 (-> 7)
    # -- a RED-only hardcode ("10 teams (was 9)") silently mislabelled crystal's
    # own "7 teams (life 7)" header (verified live, both games, both corpus
    # saves; real cartridge data, not a design invariant -- if this corpus is
    # ever replaced, re-verify these two numbers rather than trust them frozen).
    count_after_add = {"red": 10, "crystal": 7}[which]
    count_before_add = count_after_add - 1
    # BACKLOG #184 retrofit attempted here and REVERTED, now FIXED by BACKLOG #214
    # item 2: same finding as run_b89_hof's "03_menu" -- this header is
    # hof_card_text_fit() inside the gbscr card shell (source/pdna_gbhof.c ~:760,
    # "%d teams (life %d)"), the ROM's own composited tile font, not
    # source/ui_font.c/tonc's sys8Font. gb_claims.py's claim_gb= (Session._gb_rom_file()
    # extracts the embedded ROM out of THIS image) reads that font directly instead --
    # proven live on this exact frame (the brief's own target proof).
    s.shot("10_add_back_on_list", "BACKLOG #202 A2: dismissed -- back on the "
                                   "LIST card (the SAME shell), now showing "
                                   f"{count_after_add} teams (was {count_before_add}) "
                                   "with the new team "
                                   "'1: Lv5-5' on top -- the write DID land "
                                   "in-session (RAM); only the FLASH persist "
                                   "leg of gb_persist() refuses in the "
                                   "emulator build, same as every other GB "
                                   "screen's own commit",
                                   claim_gb=f"{count_after_add} teams")

    # ---- DELETE TEAM -----------------------------------------------------------------
    s.tap("START", settle=gb_shots.BIG_SETTLE)                # list -> menu (same shell)
    s.press_n("DOWN", 3, settle=gb_shots.SETTLE)              # CLEAR ALL -> ... -> DELETE TEAM
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # DELETE TEAM -> "Delete team #1?"
    s.shot("11_delete_confirm", "BACKLOG #202 F4: DELETE TEAM acts on the list "
                                 "cursor's own row directly (no second picker) -- "
                                 "'Delete team #1?'")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # yes -> gbh_delete_team -> gb_persist()
    s.run(200)
    s.shot("12_delete_refusal", "BACKLOG #202 F4: DELETE TEAM hits the SAME "
                                 "in-session-only refusal")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # dismiss -- A2: no reopen, same shell
    s.shot("13_delete_back_on_list", "BACKLOG #202 A2: dismissed -- back on "
                                      "the LIST card (the SAME shell)")

    return s


def run_b194_hof_no_rom(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #202 F1's own honest fallback: `rom` MUST be a fused image
    carrying a HoF-bearing save (Red.sav or Crystal.sav) with NO MATCHING GEN
    ROM fused at all (fuse_gb.py invoked with only the .sav payload, the same
    'no ROM at all' construction run_m1_map_gen2_no_rom() above uses for the
    Gen-2 map screen) -- app_gb_rom_path()/gb_rom_path_beside() both fail,
    gbscr_open()'s own kReasonNoRom refusal fires, and pdna_gbhof() falls back
    to hof_plain_screen() with the honest 'HALL OF FAME (GB ART: OFF)' header
    -- the SAME plain rows this screen drew before BACKLOG #194/#202 existed."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b194_{which}_norom_")
    print(f"== BACKLOG #202 F1: {which}'s HoF, no ROM fused -- the honest plain fallback ==")
    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 12)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # Records -> pdna_gbhof() -> gbscr_open()
                                                                # refuses (no ROM) -> hof_plain_screen()
                                                                # immediately, no cold-scan cost at all
    s.shot("01_plain_fallback", "BACKLOG #202 F1: no Gen ROM fused at all -- "
                                 "gbscr_open() refuses (kReasonNoRom) and "
                                 "pdna_gbhof() falls back to the ORIGINAL plain "
                                 "row list with the honest 'HALL OF FAME (GB ART: "
                                 "OFF)' title, exactly the trainer card's own D7 "
                                 "header contract")
    return s


def run_b89_hof_detail_only(core_mod, image_mod, rom: Path, out_dir: Path, which: str,
                            shot_name: str, caption: str) -> gb_shots.Session:
    """BACKLOG #89 D6/NICK proof shots: navigate straight to the team detail page
    and shoot it once, nothing else. `rom` must be the SAME kind of ONE-ROM fused
    image run_b89_hof() takes, except its embedded .sav has been edited first --
    fuse the OUTPUT of one of the recipes below, never poke the already-fused
    .gba's SAV payload directly: fuse_gb.py's directory records a CRC32 per
    payload, and poking the fused output invalidates it, which reads back as
    'not a valid save' at boot.

    BACKLOG #198 item 7: the fixtures used to come from an undocumented byte
    poke ("see this slice's own commit message for the exact offsets" -- a
    reference that named no commit and no offsets). Replaced with a
    reproducible recipe through tests/host_gbsurgery_tool.c's own --op
    hofnick/--op hofdv (BACKLOG #198 item 7's own additions, gbh_team()+
    gbh_set_mon() -- the SAME core the live EDIT MON screen edits through, not
    a hand-found file offset). Team/mon index 0/0 both games (verified live:
    Red.sav's own team 0 mon 0 is MEW, Crystal.sav's is TYPHLOSION -- real
    corpus data, not planted):
      which=red,   kind=nick:  `hgbsurg --in Red.sav --out red-nick.sav
        --op hofnick 0 0 SPARKY` -- mon 0 (MEW) nicknamed.
      which=crystal, kind=nick: `hgbsurg --in Crystal.sav --out
        crystal-nick.sav --op hofnick 0 0 FLAMETHROW` -- mon 0 (TYPHLOSION)
        nicknamed with the FULL 10-char budget (Gen 2's own raw nickname cap),
        the non-shiny worst case for the fit-vs-truncate caption below.
      which=crystal, kind=shiny (R2 re-verify): `hgbsurg --in Crystal.sav
        --out crystal-shiny.sav --op hofdv 0 0 atk 2 --op hofdv 0 0 def 10
        --op hofdv 0 0 spe 10 --op hofdv 0 0 spc 10 --op hofnick 0 0
        FLAMETHROW` -- mon 0 (TYPHLOSION) BOTH shiny-capable (g2_dv_shiny's
        own Atk&2/Def=Spe=Spc=10 quad -- atk=2 satisfies the Atk&2 bit test,
        atk=0 does NOT, confirmed live: atk=0 read back shiny=0) AND
        nicknamed at Lv 100 -- the exact worst-case suffix (" Lv.100 *", 9
        chars + NUL) that overflowed the old char[8] (R2).
    Each recipe verified live end to end: hgbsurg's own exit code 0, then a
    throwaway gbh_team()-dump readback confirmed the exact nick/dv/shiny
    fields landed, before ever fusing or booting."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b89_{which}_")
    print(f"== BACKLOG #89 D6/NICK: {which}'s Hall of Fame detail, poked-.sav proof shot ==")
    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 12)
    # BACKLOG #198 item 7 fix: this was BIG_SETTLE, but Records -> pdna_gbhof() is a
    # COLD gbscr_open() scan (the FIRST shell open this session, same ~3,240-frame
    # PDNA_DELTA whole-ROM locator cost run_b89_hof()'s own docstring measures) --
    # BIG_SETTLE (40 frames) left the nav menu on screen, cursor still on Records
    # (found live re-verifying this function for item 7: the shot showed the MENU,
    # not the HoF card). GB_ART_COLD_SETTLE, the SAME constant run_b89_hof() already
    # uses for this exact same first-entry cost, is what actually rides it out.
    s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Records -> pdna_gbhof() -> FIRST gbscr_open()
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # row 1 -> the team detail (same open shell)
    s.shot(shot_name, caption)
    return s


def run_b90_fly(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #90: the Gen-1/2 Fly-destination screen (source/pdna_gbfly.c) over
    gb_fly.h's bitfield core -- the same shape as run_u4_bag()/run_u5_pack() above,
    reused for a plain (non-gbscreen-shell) list screen. `rom` must be a ONE-ROM
    fused image (Red-only for `which == "red"`, Crystal-only for `which ==
    "crystal"` -- same BACKLOG #98 harness-gap reasoning as U4/U5's own images), so
    the boot picker is skipped (gb_delta_pick_save()'s `if (n == 1) return 0`) and
    one A tap reaches the box grid directly (#279: no info page).

    Nav: A -> box grid (#279: no info page) -> START -> nav menu -> DOWN x14 (Party=0, Bank=1,
    Daycare=2, Trainer=3, Clock fix=4, Mirage=5, Pokedex=6, Bag=7, Flags&counters=8,
    Bases=9, Blocks=10, Tickets=11, Records=12, Frontier=13, Fly=14 -- PDNA_NAV_ITEMS
    order, source/pdna_layout.h) -> A -> pdna_gb_fly()."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b90_{which}_")
    print(f"== BACKLOG #90: {which}'s own Fly destinations (single-ROM image -> "
          "standalone -> Fly) ==")

    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 14)                                    # Party -> ... -> Fly (index 14)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Fly -> pdna_gb_fly()
    s.shot("01_list", f"BACKLOG #90: {which}'s own Fly destinations -- the "
                       "ON/x count header, one row per gbfy_count() destination, "
                       "cursor on row 1")

    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("02_row2", "BACKLOG #90: DOWN moves the cursor to row 2")

    s.tap("A", settle=gb_shots.SETTLE)                      # toggle the selected row
    s.shot("03_toggled", "BACKLOG #90: A toggles the selected destination -- the "
                          "row's ON/off text and the header count both flip; "
                          "rmbl_fire(RCUE_EDIT) is the same edit haptic every "
                          "other GB screen's own field edits use")

    # D4 (BACKLOG #90): START's mark-all. Fires from the SAME cursor position
    # 03_toggled left the toggled row on, still row 2 -- START asks a confirm, then
    # sets every row not already ON (skipping Gen 2's four spawn-only rows via the
    # "spn" fly_tag() predicate), same shape as the Gen-3 Fly screen's own START.
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # Fly's own START (not the box grid's)
    s.shot("04_start_confirm", "BACKLOG #90: START -> 'Mark all destinations? "
                                "Skips story order.' (app_confirm), the same "
                                "one-shot gate pdna_fly.c's own START uses")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # confirm -> mark-all loop -> result panel
    s.shot("05_start_result", "BACKLOG #90: the '%d newly marked.' result panel"
                               + (" -- 'Spawn-only rows untouched.' on Gen 2 (the "
                                  "four spn-tagged rows are never a real Fly-menu "
                                  "destination, so mark-all leaves them exactly as "
                                  "found)" if which == "crystal" else "")
                               + " (the s_msg-over-app_confirm ghosting fix, BACKLOG #119, "
                                 "verified: no dialog residue behind the panel)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss the result panel -> back to the grid

    if which == "crystal":
        # Gen 2 only: scroll to a spawn-only row (index 0/1/17/27 -- HOME/DEBUG/
        # UNION_CAVE/FAST_SHIP, see pdna_gbfly.h's own header note) and show its
        # "spn" tag plus the footer legend explaining it.
        # sel is cyclic (`sel = sel ? sel - 1 : n - 1`): row 2 (idx1) -> UP -> row 0
        # (idx0) -> UP -> WRAPS to the LAST row (idx27, Fast Ship), not back to row 0
        # -- Fast Ship and Union Cave (idx17) are both spawn-only ("spn"-tagged), same
        # as row 0 (Spawn: Home) and row 1 (Spawn: Debug) would have been.
        s.press_n("UP", 2, settle=gb_shots.SETTLE)          # row 2 -> row 0 -> wraps to row 27 (Fast Ship)
        s.shot("06_spawn_only_tag", "BACKLOG #90: row 27 (Fast Ship) carries the "
                                     "'spn' tag and the footer legend 'spn = no effect "
                                     "in game' -- flypoints.asm's own Fly menu "
                                     "never offers this bit as a destination even "
                                     "though wVisitedSpawns has a real bit for it -- "
                                     "still off after D4's mark-all, proving the "
                                     "skip really held")
        s.press_n("DOWN", 2, settle=gb_shots.SETTLE)        # back to row 2 (the toggled row)

    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B with a real pending edit -> commit prompt
    s.shot("07_commit_prompt", "BACKLOG #90: B with a real pending edit -> 'Save "
                                "fly destinations?' (app_confirm), the same dialog "
                                "every other GB screen's own commit uses")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A = yes -> gb_persist() -> PDNA_DELTA refusal
    s.shot("08_confirmed", "BACKLOG #90: A confirms -- gb_persist()'s PDNA_DELTA "
                            "branch refuses ('Edits are in-session only in the "
                            "emulator build.') because this build has no SD card "
                            "for a GB image at all (same #62 D2/D5 branch every "
                            "other GB screen's own commit hits here); the write "
                            "path itself is proved by the retail gate's fly case "
                            "(tools/gb_retail_gate.py), not this shot")

    return s


# source/osk.c's QWERTY rows (both cases, no shift mode) -- kept as a literal copy
# for the SAME reason g3_shots.py's own osk shot does not need one (it never types
# a full name): this one does, so it needs to know which (row, col) cell holds each
# glyph. Any drift from source/osk.c's own KB[] would only make this driver type
# the wrong letters, never break the app itself -- osk.c stays the single source of
# truth for what actually ships.
_OSK_KB_ROWS = [
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm",
    "QWERTYUIOP",
    "ASDFGHJKL",
    "ZXCVBNM",
    " -.,'!?",
]


def _osk_type_char(s: "gb_shots.Session", pos: list[int], ch: str) -> None:
    """Drive the on-screen keyboard's cell cursor from `pos` (mutated in place --
    [row, col], the SAME two ints osk_core's own `cr`/`cc` track) to `ch`'s cell via
    DOWN/RIGHT only (osk.c's cr/cc both wrap, so any cell is reachable without ever
    needing UP/LEFT), then A to insert it at the current caret position. Mirrors
    osk_core's OWN per-loop clamp (`if (cc >= rowlen(cr)) cc = rowlen(cr) - 1;`) when
    a row change lands on a column past the new row's shorter length."""
    target = None
    for r, row in enumerate(_OSK_KB_ROWS):
        c = row.find(ch)
        if c >= 0:
            target = (r, c)
            break
    if target is None:
        raise ValueError(f"_osk_type_char: {ch!r} is not on any osk.c KB row")
    tr, tc = target
    cr, cc = pos
    downs = (tr - cr) % len(_OSK_KB_ROWS)
    if downs:
        s.press_n("DOWN", downs, settle=gb_shots.SETTLE)
    cr = tr
    if cc >= len(_OSK_KB_ROWS[cr]):
        cc = len(_OSK_KB_ROWS[cr]) - 1   # osk_core's own clamp, applied before the next move
    rights = (tc - cc) % len(_OSK_KB_ROWS[cr])
    if rights:
        s.press_n("RIGHT", rights, settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.SETTLE)    # KEY_A -> u8w_apply_key(..., U8W_OP_INSERT, KB[cr][tc])
    pos[0], pos[1] = cr, tc


def run_b90_boxname(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """F1 step 3 (BACKLOG #94): the Gen-2 box-rename banner, over gb_boxnames.h's
    gbbn_rename/gbbn_supported (source/pdna_gen12.c's gbsrc_set_name_impl/
    gbsrc_can_rename_impl, the guard-split pair this slice wired). `rom` must be a
    Gen-2-only ONE-ROM fused image (Gold.gbc+Gold.sav or Crystal.gbc+Crystal.sav,
    same BACKLOG #98 harness-gap reasoning as --b90-fly) -- Gen 1 has no box-name
    table at all (gbbn_supported refuses it), so this shot is Gen-2 only by design,
    not a coverage gap.

    Nav: A -> box grid (#279: no info page, cur=0, on_title=false) -> UP (on_title=true, the
    banner) -> A -> gbsrc_can_rename_impl() true (g_ed exists on this resident-image
    entry, app_can_edit() true even in the emulator -- only the FINAL write is
    Omega-gated, not the in-RAM edit -- and gbbn_supported() true on a Gen-2 save)
    -> osk_input("BOX NAME", <current name>, ...) opens, seeded and caret-at-end.

    The real sequence differs from a same-shaped Fly/Bag/Pack commit in ONE way this
    slice's own design calls for: gbsrc_set_name_impl() calls gb_persist("boxname")
    itself, directly inside src->set_name() (BEFORE pdna_box.c's own `src->commit()`
    even runs) -- so the PDNA_DELTA refusal's msg_wait (source/pdna_main.c, "Press
    A" to dismiss, NOT B) fires the instant osk_input's own START confirms the new
    name, and the box grid does not repaint with the RENAMED banner until THAT
    dialog is dismissed. Order on screen is therefore: seeded keyboard -> (backspace
    the old name, type a new one, START) -> persist-refusal dialog -> A dismisses it
    -> THEN the renamed banner."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b94_{which}_")
    print(f"== BACKLOG #94: {which}'s own box-rename banner (single-ROM image -> "
          "standalone -> box grid -> banner -> A -> osk_input) ==")

    boot_to_gb_session(s, rom, which=which)
    s.shot("01_box_grid", f"BACKLOG #94: {which}'s own box grid, freshly opened -- "
                           "box 0, cursor on the top-left cell")

    # This transition's own repaint (draw_box_banner's selection frame + draw_footer's
    # on_title hint) lands slower than BIG_SETTLE's 40 frames on this image -- measured
    # empirically (40 frames: still the pre-UP banner/footer; 200: fully repainted) --
    # so it gets its own longer settle rather than a shared constant tuned for the
    # cheaper cursor moves the rest of this file uses BIG_SETTLE for.
    s.tap("UP", settle=200)                                   # cur < COLS -> on_title = true
    s.shot("02_banner_selected", "BACKLOG #94: UP from the top row selects the "
                                  "TITLE row (on_title = true) -- A on the banner "
                                  "is the rename shortcut (BACKLOG #33), same one "
                                  "the Gen-3 box screen already has")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # can_rename true -> osk_input opens
    s.shot("03_osk_seeded", "BACKLOG #94: A on the banner -> gbsrc_can_rename_impl() "
                             "allows it (a live Gen-2 session, an Omega-writable "
                             "cart, gbbn_supported() true) -> osk_input('BOX NAME', "
                             "...) opens seeded with the box's CURRENT name, caret "
                             "already past the last character")

    pos = [0, 0]                                              # osk_core's own cr=0, cc=0 start
    for _ in range(8):                                        # box names cap at 8 chars -- always
        s.tap("B", settle=gb_shots.SETTLE)                    # enough backspaces to clear any seed
    for ch in "TEST":
        _osk_type_char(s, pos, ch)
    s.tap("START", settle=gb_shots.BIG_SETTLE)                # confirm -> set_name() -> gb_persist()
    s.shot("04_persist_refusal", "BACKLOG #94: START confirms the new name 'TEST' "
                                  "-- gbsrc_set_name_impl() writes it via "
                                  "gbbn_rename() THEN calls gb_persist('boxname') "
                                  "itself, so the PDNA_DELTA refusal (source/"
                                  "pdna_main.c's msg_wait, 'Edits are in-session "
                                  "only in the emulator build.', dismissed with A) "
                                  "fires HERE, before the box grid ever repaints -- "
                                  "the write already landed in g_ed->img regardless "
                                  "(same #62 D2 posture every other GB screen's own "
                                  "commit has); the real write is proved by the "
                                  "retail gate's boxname case (tools/"
                                  "gb_retail_gate.py), not this shot")

    s.tap("A", settle=200)                                    # dismiss msg_wait -> box grid repaints
    s.shot("05_renamed_banner", "BACKLOG #122: back at the box grid -- the banner now reads 'TEST' (verbatim, no 'GB ' prefix on Gen-2 renamed boxes); gbsrc_get_name() re-reads g_m->g2names and pdna_gen12_box_name() echoes it as-is for Gen-3 parity; the SAVE holds the undecorated 'TEST'.")

    return s


def run_b132_portrait(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> list[gb_shots.Session]:
    """BACKLOG #132: proves the fix (sprite_era.c:138's se_resolve_for_router() now
    exempts SE_KIND_GEN1/SE_KIND_GEN2 from the NATIVE collapse) by driving box 1
    slot 1's own VIEW summary through BOTH mounts that reach pdna_origin_art_
    portrait() -- the standalone/boot-picker mount (boot_to_gb_session(), a single-
    save fused image skips the top-level picker entirely) and the nested START >
    NV_GB import mount (RETIRED by BACKLOG #239/#273 -- see the "Mount 2" comment in
    the body; that half now only proves the "BANK ONLY" refusal). `rom` must be the
    COMBINED image (Emerald.sav + Red/Gold/Crystal, `make delta-gb`'s own recipe --
    tools/fuse_gb.py) so BOTH mounts are reachable from the one image: the top-level
    picker's own `which` row for the boot-picker mount, and the Emerald row -> NV_GB
    -> `which` row for the nested mount.

    Each mount's own box grid is shot BEFORE opening the cell's menu -- this is the
    "box-grid hover panel" the brief asks for: root-cause point 7 says the box grid
    is unaffected by this bug because it asks se_resolve_cell(), not
    se_resolve_for_router(), so this shot is the pixel-identical-before/after
    control for the grid half of the screen, alongside the summary shot proving the
    PORTRAIT half actually changed (Gold) or stayed put (Red).

    Returns both Sessions (boot-picker mount, nested mount) so the caller can sum
    `.taken`/`.skipped` across both."""
    gb_shots.assert_vehicle(rom, "ART")  # BACKLOG #255 review: its docstring says `make delta-gb`'s own recipe -- the full-art delta
    sessions: list[gb_shots.Session] = []

    # Mount 1: the standalone/boot-picker mount -- a combined image's top-level
    # "PICK A SAVE" picker, `which`'s own row, straight to that save's box grid
    # (boot_to_gb_session() handles the picker-vs-no-picker fork itself).
    s1 = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b132_boot_{which}_")
    print(f"== BACKLOG #132: {which}'s own summary portrait, boot-picker mount ==")
    boot_to_gb_session(s1, rom, which=which)
    s1.shot("01_box_grid", f"BACKLOG #132: {which}.sav's box grid, box 1, cursor on "
                            "slot 1 -- the box-grid hover panel/cell art, reached "
                            "via se_resolve_cell() (root-cause point 7: this hook "
                            "does NOT collapse, so this shot is the unaffected "
                            "control against the summary shot below)")
    s1.tap("A", settle=gb_shots.BIG_SETTLE)                   # slot 1 -> the occupied-cell menu
    s1.shot("02_mon_menu", f"BACKLOG #132: {which}.sav -- the occupied-cell menu "
                            "this standalone mount offers (VIEW/EDIT, ITEM, MOVE TO "
                            "BOX, ...)")
    s1.tap("A", settle=gb_shots.BIG_SETTLE)                   # VIEW/EDIT -> the native summary
    s1.shot("03_view_summary", f"BACKLOG #132: {which}.sav -- VIEW: the summary "
                                "portrait via pdna_origin_art_portrait(). For Gold "
                                "this is the fix itself (router_era now GEN2, not "
                                "collapsed to NATIVE -- the portrait is Gold's own "
                                "4-shade GB sprite, not the compiled Gen-3 stand-"
                                "in); for Red this must be pixel-identical to a "
                                "pre-fix capture (SE_KIND_GEN1's blind o.gen=1 "
                                "verdict already happened to be right, so this "
                                "record moves from the legacy branch to the era "
                                "branch without changing a pixel).")
    sessions.append(s1)

    # Mount 2 (BACKLOG #273): the nested START > NV_GB import mount was CLOSED on
    # purpose by BACKLOG #239 (A on NV_GB from a live Gen-3 save now shows the "BANK
    # ONLY" dialog and mounts nothing), so the "second, independent proof" through it
    # no longer exists and is RETIRED. This session instead pins that fact -- the same
    # navigation the old nested half drove now must land on the refusal, so if the
    # mount is ever reopened this chain fails loudly rather than silently shooting it.
    s2 = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b132_nested_{which}_")
    print(f"== BACKLOG #132/#273: {which}'s nested NV_GB mount is retired -- refusal only ==")
    s2.run(700)
    s2.tap("A", settle=gb_shots.BIG_SETTLE)                   # top-level picker, Emerald row (default) -> box
    s2.tap("START", settle=gb_shots.BIG_SETTLE)               # box screen -> nav menu
    s2.tap("RIGHT")                                            # column 0 -> column 1
    s2.press_n("DOWN", nav_down_from_col_top("NV_GB"))         # Blocks -> ... -> GB import
    s2.tap("A", settle=gb_shots.BIG_SETTLE)                   # NV_GB -> refuse, not a mount
    s2.shot("01_nested_refused", f"BACKLOG #273: the nested NV_GB mount that used to "
                                  f"carry {which}.sav's second portrait proof is closed "
                                  "(BACKLOG #239) -- A on GB import shows the 'BANK ONLY' "
                                  "dialog; the boot-picker mount above is the only "
                                  "portrait proof now",
            claim=["BANK ONLY", "Open the GB save on its own,", "send it to the Bank, come back."])
    s2.tap("A", settle=gb_shots.BIG_SETTLE)                   # msg_wait dismisses on A
    sessions.append(s2)

    return sessions


def run_b85_daycare(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #85 (source/pdna_gbdaycare.c): the Gen-1/2 Day-Care screen over
    gb_daycare.h's core -- the Gen-1/2 twin of pdna_main.c's pdna_daycare(). `rom`
    must be a ONE-ROM fused image (tools/fuse_gb.py fed only Red.gb+Red.sav for
    `which="red"`, or only Gold.gbc+Gold.sav for `which="gold"` -- same single-ROM
    requirement run_u4_bag()/run_u5_pack() document, BACKLOG #98's known harness gap).

    RE-SHOT for the DO-NOT-SHIP review's D1 (Put-in moves, not pastes) and D6
    (Take-out lands party-first, else the first box with room, else refuses). The
    original shots entered on BOX1 (index 0, 20/20 on BOTH corpus saves), so every
    Take-out landed straight into GBS_ERR_FULL -- a refusal this file's own caption
    mis-described as the intended behaviour. This run instead enters on the FIRST
    box with a free slot on each corpus save (verified directly with
    tests/host_gbsurgery_tool.c's own --list, matching the review's own measured
    boxes):
      Red.sav   box index 4  (in-game BOX5,  19/20, HITMONLEE at slot 0)
      Gold.sav  box index 12 (in-game BOX13, 17/20, MILTANK   at slot 0)
    so the Take-out SUCCESS path is reachable. R press count == the target box
    index exactly on this fixture (R x4 for Red, R x13 for Gold -- confirmed by
    direct probe, matching this file's own pre-existing "drop-prone input" note
    for R/DOWN/RIGHT: a shorter settle silently swallowed a press once already).
    Both corpus parties are FULL (6/6), so the landing search falls through to
    "first box with room" for both games -- the mon lands back in the SAME box it
    came from (which is exactly right: that box is still the first one with a free
    slot after the Put-in reduced its count by one).

    Nav: A -> box grid (#279: no info page, rom_gbsprite cold scan) -> R x(box index) ->
    START -> nav menu -> DOWN x2 (Party->Bank->Daycare, PDNA_NAV_ITEMS index 2) ->
    A -> pdna_gbdaycare(cur_box = the box just selected). The screen itself draws
    no icons (this slice's own header note: the animated yard art is Gen-3-only,
    private to pdna_main.c, out of this file list's reach), so BIG_SETTLE covers
    every transition inside it -- no GB_ART_COLD_SETTLE needed past the box-grid
    entry.

    The egg case is NOT attempted here: producing an `egg_ready` state means
    actually breeding two compatible Pokemon in-game (steps of overworld movement),
    which neither this script nor host_gbsurgery_tool.c's `--op daycare` (deposit
    only) can fabricate without new surgery-tool support outside this slice's file
    list -- flagged as hardware/gameplay-only rather than faked with an edited
    fixture this script cannot itself produce. A genuine "everything is full"
    Take-out refusal is ALSO not reproducible from this real corpus (some box
    always has room once the party is full) -- the refusal shown instead is the
    new D1 confirm-decline path (pressing B at "Send to Day-Care?"), a real,
    reachable, zero-byte-diff refusal this screen did not have before the review."""
    box_index = 4 if which == "red" else 12
    box_label = "BOX5" if which == "red" else "BOX13"
    src_mon = "HITMONLEE" if which == "red" else "MILTANK"
    landed_label = "Box 5" if which == "red" else "Box 13"

    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b85_{which}_")
    print(f"== BACKLOG #85: {which}'s own Day Care (D1/D6 re-shoot) ==")

    boot_to_gb_session(s, rom, which=which)
    s.shot("00_box_before", f"BACKLOG #85: {which}.sav's own BOX1 on entry, before "
                             "switching to the box this run enters the Day Care from")

    # R press count == the target box index EXACTLY on this fixture (confirmed by
    # direct probe on BOTH corpus saves). settle=300, not the default BIG_SETTLE=40
    # and not the 250 an earlier revision of this same function used: this exact
    # input (R, held rapid-fire) is the one this codebase has already found
    # drop-prone once (tools/dgb_shots.py's own R1-xfer comment) -- a probe at 250
    # on Gold.sav's own image silently dropped one of the 12 presses needed to
    # reach index 12 (landing on BOX12/20/20, one short and already full), while
    # the SAME 12 presses at 300 landed correctly on BOX13/17/20 every time this
    # was re-checked. 300 also re-confirmed correct on Red.sav.
    s.press_n("R", box_index, settle=300)
    s.shot("01_box_selected", f"BACKLOG #85: {which}.sav's own {box_label} selected -- "
                              f"the first box with a free slot ({src_mon} at slot 0), "
                              "so `cur_box` (what the Day-Care's own Put-in/Take-out "
                              "picker and landing search will use) is this box")

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 2)                                     # Party -> Bank -> Daycare (index 2)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Daycare -> pdna_gbdaycare()
    s.shot("02_screen", f"BACKLOG #85: {which}'s own Day Care on entry, cur_box = "
                        f"{box_label} -- the panel/menu shape mirrored from "
                        "pdna_daycare(), no yard art")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on slot 0 -> the action popup
    s.shot("03_menu_slot0", "BACKLOG #85: the per-slot action popup on slot 0 (View/Edit + "
                             "Take out if occupied, or Put in if empty, Cancel)")

    # Slot 0 starts empty on both fixtures' own Day Care, so the popup's first row
    # is "Put in" -- A opens cur_box's picker (gbdc_pick).
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Put in -> gbdc_pick over cur_box
    s.shot("04_pick_list", f"BACKLOG #85: the deposit picker -- {box_label}'s own occupied, "
                            "non-Egg slots (gbdc_pick, the smallest 'pick one "
                            "owned mon' list this slice could build, since "
                            "gb_daycare's own gbd_deposit() requires a "
                            "box-shaped record -- see pdna_gbdaycare.h)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick the top row -> D1's new confirm prompt
    s.shot("05_confirm", "BACKLOG #85 D1: the new confirm prompt (app_confirm, "
                          "'Send to Day-Care? / Moves this Pokemon there.') -- "
                          "gbdc_deposit() did not ask before this review; this is "
                          "the same review fix as pdna_main.c's own app_to_daycare()")

    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # DECLINE -> the D1 refusal/cancel path
    s.shot("06_confirm_declined", "BACKLOG #85 D1: B at the confirm backs out -- gbd_deposit() "
                                   "never ran, gbs_delete() never ran, nothing changed. "
                                   "The only reachable 'refusal' shot in this run: a genuine "
                                   "full-boxes-and-full-party Take-out refusal cannot be "
                                   "built from this real corpus (some box always has room "
                                   "once the party's own 6/6 is accounted for)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on slot 0 again -> Put in again
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # gbdc_pick over cur_box again
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick the top row -> the confirm again
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ACCEPT -> gbd_deposit() + gbs_delete() + gb_persist()
    s.shot("07_persist_notice", "BACKLOG #85: gb_persist()'s own PDNA_DELTA branch: 'Edits "
                                 "are in-session only in the emulator build' -- "
                                 "this build has no SD card, so every write shows "
                                 "this notice and RETURNS FALSE, which is why "
                                 "gbdc_deposit()'s own 'LEFT AT DAY CARE / Moved from "
                                 "the box. Saved.' message (real, shipped text) is "
                                 "never reachable in this build -- not specific to "
                                 "this screen, same gate every other GB write path "
                                 "in this app hits")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss the notice -> back at the Day Care screen
    s.shot("08_deposited", f"BACKLOG #85 D1: back at the Day Care screen -- slot 0 now shows "
                            f"{src_mon} (moved, not copied)")

    # settle=120, not BIG_SETTLE=40: a direct probe found the box-count banner still
    # blank at 40 frames after this specific transition (Day-Care -> B -> box grid,
    # a re-entry path no earlier shot list in this file exercised) -- the grid's own
    # mon tiles redraw in time but the "N:GB BOXN cc/20" banner text needs longer.
    s.tap("B", settle=120)                                  # back out to the box grid
    s.shot("09_source_emptied", f"BACKLOG #85 D1: back at {box_label} -- its own count "
                                 "dropped by exactly one and slot 0 no longer shows "
                                 f"{src_mon} (D1's own repro: a Put-in that PASTES "
                                 "instead of MOVES would leave this box unchanged)")

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu again
    s.press_n("DOWN", 2)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # back into the Day Care
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the now-occupied slot 0
    s.shot("10_menu_occupied", "BACKLOG #85: the action popup on an OCCUPIED slot -- now "
                                "View/Edit + Take out, no Put in row")
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # View/Edit -> Take out
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Take out -> gbdc_take: withdraw + land + gb_persist()
    s.shot("11_take_out_success", "BACKLOG #85 D6: the SUCCESS case underneath this same "
                                  "generic PDNA_DELTA notice -- both corpus parties are "
                                  "full (6/6), so gbdc_land()'s own search falls through "
                                  f"to the first box with room, which is {box_label} "
                                  "itself (still has a free slot after the Put-in above). "
                                  f"gbdc_take()'s own success text ('Sent to {landed_label}.') "
                                  "is real and shipped but, like the deposit case above, "
                                  "unreachable in this SD-less emulator build -- the STATE "
                                  "change it names is verified by slot 0 emptying (12) and "
                                  f"is hardware-only to see spelled out on screen")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -> back at the Day Care screen
    s.shot("12_slot0_empty_again", "BACKLOG #85 D6: back at the Day Care screen -- slot 0 is "
                                    "empty again (the Take-out landed the mon back in the "
                                    "box, not a phantom copy left behind)")

    s.tap("B", settle=120)                                  # back out to the box grid (same
                                                              # settle note as 09 above)
    s.shot("12b_box_restored", f"BACKLOG #85 D6: back at {box_label} -- its count is back "
                                f"up to what it was before the Put-in (one more than 09's "
                                f"own shot); gbs_insert() appends at the box's own next "
                                f"free slot, not necessarily slot 0, so {src_mon} is "
                                "back in this box but not necessarily in the same cell -- "
                                "the count is gbdc_land()'s own visual proof, not the "
                                "grid position")

    if which != "red":
        s.tap("START", settle=gb_shots.BIG_SETTLE)          # re-enter for the Gen-2-only leg below
        s.press_n("DOWN", 2)
        s.tap("A", settle=gb_shots.BIG_SETTLE)
        # Back at the main screen, no popup open.
        s.tap("DOWN", settle=gb_shots.SETTLE)               # slot 0 -> slot 1 (the Lady's)
        s.shot("13_slot1_cursor", "BACKLOG #85: Gen 2 only: DOWN moves the row cursor to the "
                                   "second slot -- Gen 1 has no second slot to move to")
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # A on slot 1 -> its own popup
        s.shot("14_menu_slot1", "BACKLOG #85: the same action popup, now targeting slot 1 -- "
                                 "still empty, so Put in only")
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # Put in -> gbdc_pick over cur_box again
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # pick the top row -> the confirm
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # ACCEPT -> deposit + delete -> gb_persist()
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # dismiss the emulator-build notice
        s.shot("15_slot1_deposited", "BACKLOG #85: slot 1 (the Lady's) deposited too -- "
                                      "Man's slot is empty here (it was put in AND taken "
                                      "back out in the D6 demo above), so this shows the "
                                      "one-Pokemon-boarding status text rather than the "
                                      "two-slot compatibility read; that panel wording is "
                                      "unrelated to this pass's D1/D6/D8 fixes and was "
                                      "already exercised by BOTH slots occupied in this "
                                      "same file's earlier BACKLOG #85 shot list")

    return s


def run_b114_yard(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #114: the Day-Care YARD on the Gen-1/2 screen (source/pdna_gbdaycare.c,
    pdna_yard.h) -- dc_scene()/dc_icon_over_bg()/dc_pointer() now draw underneath the
    SAME status panel BACKLOG #85's own shots already covered mechanically (Put in /
    Take out / the confirm prompts); this run is about the SCENE, not the mechanics.

    `rom` must be a ONE-ROM fused image, same requirement as run_b85_daycare (no Gen-3
    ROM is ever registered in a PDNA_DELTA build -- app_register_rom(), the browse-for-
    ROM flow, is `#ifndef PDNA_DELTA` entirely, since there is no SD to browse in the
    emulator; confirmed by direct probe, not assumed). app_yard_visitors_ok() is
    therefore ALWAYS false here (g_yard_visitors also defaults off), so every shot
    below shows the "No visitors: register a Gen-3 ROM" panel row and NO invented
    visitor icons -- the brief's own sanctioned fallback ("visitors present ... else
    the 'no visitors' line"). The Gen-1 visitor cap (<=151, vs Gen 2/3's 251) is
    proven on the host instead (tests/host_yard_test.c), the same posture every other
    "cannot fabricate this state in the emulator" case in this file already takes.

    Real boarder icons ALSO do not render here (mon_icon_for_form_frame's Gen-3-keyed
    icon cache has no source without a registered Gen-3 ROM -- a DIFFERENT lookup path
    than the GB-native rom_gbicon one the box grid already proves works in this same
    fused image, BACKLOG #85's own 00_box_before shot) -- the yard degrades to its
    plain background with no icon, never a blank screen, exactly BACKLOG #114's own
    acceptance line for the artless/no-ROM case.

    Same box-index-to-reach-a-free-slot setup as run_b85_daycare (box 4/HITMONLEE for
    Red, box 12/MILTANK for Gold)."""
    box_index = 4 if which == "red" else 12
    box_label = "BOX5" if which == "red" else "BOX13"

    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b114yard_{which}_")
    print(f"== BACKLOG #114: {which}'s own Day-Care YARD ==")

    boot_to_gb_session(s, rom, which=which)
    s.press_n("R", box_index, settle=300)                   # -> the box with a free slot (see run_b85_daycare)

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 2)                                     # Party -> Bank -> Daycare (index 2)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Daycare -> pdna_gbdaycare()
    s.shot("01_yard_empty", f"#114: {which}'s own Day-Care yard on entry, both slots "
                             "empty -- dc_scene()'s background (the procedural scene "
                             "in this artless image) fills the screen where the two "
                             "text rows used to be; the panel's third row already "
                             "reads 'No visitors: register a Gen-3 ROM' (no ROM is "
                             "ever registered in this emulator build); the footer "
                             f"names the selected empty slot ('{'Boarder' if which == 'red' else 'Man'} "
                             "(empty)') since there is no icon in the yard to point at")

    # NOTE (brief's step 6 asked for a "SELECT scale both ways" shot): checked and
    # dropped -- gb_scale_mode's SELECT toggle (source/pdna_gbscreen.c:157) belongs
    # to the gbscr_run_demo SHELL (the font/card/pack tile-blit viewers reached via
    # Settings' hidden SELECT key, see run_gbscreen_shell() above), which pdna_gbdaycare
    # never routes through -- it draws with ui_*/Mode-3 calls directly and its own key
    # mask has never included KEY_SELECT, before or after this backlog item. Pressing
    # SEL here produced a pixel-IDENTICAL frame (verified: this script's own
    # consecutive-differ check caught it), confirming there is no scale toggle on this
    # screen to demonstrate -- adding one would be a new feature outside BACKLOG #114's
    # six described steps, not a screenshot of existing behaviour.

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the selected empty slot -> the popup
    s.shot("02_popup_over_yard", "#114: the per-slot action popup (gbdc_menu) drawn "
                                  "OVER the yard scene -- the popup geometry is "
                                  "unchanged (PDNA_DCPOP_*), it now sits on top of "
                                  "background art instead of a blank fill")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Put in -> gbdc_pick over cur_box
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # pick the top row -> the confirm
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ACCEPT -> deposit + delete -> gb_persist()
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss the emulator-build notice
    s.shot("03_one_boarder", f"#114: {box_label}'s own mon deposited -- 'Boarding "
                              f"1/{'1' if which == 'red' else '2'}' up top and the "
                              "footer drops '(empty)' now that the selected slot is "
                              "occupied, even though the icon itself does not render "
                              "here (no registered Gen-3 ROM -- see this function's "
                              "own header comment); the yard degrades gracefully, "
                              "it does not go blank")

    if which != "red":
        s.tap("B", settle=120)                              # back to the box grid (same settle
                                                              # note as run_b85_daycare's own re-entries)
        s.tap("START", settle=gb_shots.BIG_SETTLE)
        s.press_n("DOWN", 2)
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # back into the Day Care
        s.tap("DOWN", settle=gb_shots.SETTLE)               # Man's slot -> Lady's slot
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # A on the Lady's empty slot -> popup
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # Put in -> gbdc_pick over cur_box
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # pick the top row -> the confirm
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # ACCEPT -> deposit + gb_persist()
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # dismiss the emulator-build notice
        s.shot("04_two_boarders", "#114 Gen 2 only: both slots now occupied -- "
                                   "'Boarding 2/2', the panel reads the game's own "
                                   "compatibility flag instead of the single-boarder "
                                   "text, and the footer names whichever slot is "
                                   "currently selected as occupied (no more "
                                   "'(empty)')")

    return s


def run_d7_gold(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """N6(f): Gold's own START > BAG refusal, from a COMMITTED driver (`rom` must
    be a Gold-only fused image, tools/fuse_gb.py fed Gold.gbc+Gold.sav -- the
    earlier docs/shots/u4/d7_gold_01_bag_refusal.png came from an ad-hoc,
    non-driver capture; this reproduces it from code). Gen 2's NV_BAG row is
    NAV_COMING_SOON (source/nav_avail.c: 'The Bag is coming soon.') -- Gen-1's own
    Bag screen (pdna_gbbag.c) is Gen-1 only, Gen 2's Pack is a later slice.
    Nav: gb_delta_pick_save()'s own `if (n == 1) return 0` (source/pdna_main.c
    ~8913) means a SINGLE-ROM fused image (this one -- only Gold, no Emerald/
    Gen-3 save fused, so gb_delta_boot_pick()'s own picker never triggers either,
    same gate) skips straight to the GB save's box grid with NO boot-picker
    taps at all (#279: no info page) -- unlike run_u4_bag()'s own doc comment above
    (which describes the combined multi-ROM image's picker), this image needs exactly
    ONE tap (A -> box grid), not DOWN+A+A. Then: START -> nav DOWN x7
    (Party->Bank->Daycare->Trainer->Clock->Mirage->Pokedex->Bag, SAME NV_* row
    order as Gen 1 -- nav_avail's GB_TABLE has one row per NV_* id with a
    per-generation COLUMN, not a per-generation row order) -> A -> app_nav_refuse()."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "d7_gold_")
    print("== N6(f): Gold's own START > BAG refusal ==")
    boot_to_gb_session(s, rom, which="gold")
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 7)
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("01_bag_refusal", "N6(f): Gold, START > (nav DOWN x7) > Bag -> "
                              "app_nav_refuse() -- 'COMING SOON' / 'The Bag is "
                              "coming soon.' (nav_avail.c's own GB_TABLE[NV_BAG][1] "
                              "for SE_KIND_GEN2). Gen-1's own pdna_gbbag.c screen "
                              "is Gen-1 only (pdna_gbbag() itself refuses "
                              "s->gen != GB_GEN1); Gen 2's Pack is a later slice, "
                              "not this one.")
    return s


def run_b86_clock(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #86/#108: pdna_gbclock.c's own Clock screen -- reached via the standalone
    mount's nav menu, NV_CLOCK (index 4, column 0: Party->Bank->Daycare->Trainer->Clock
    fix, DOWN x4 from a fresh menu, same arithmetic as run_d7_gold's own NV_BAG DOWN x7
    -- see source/pdna_main.c nav_menu(): col = i / PDNA_NAV_ROWS, row = i % PDNA_NAV_ROWS,
    PDNA_NAV_ROWS == 10, so index 4 stays in column 0 and needs no RIGHT press at all).

    `rom` MUST be a Crystal-ONLY fused image (tools/fuse_gb.py fed Crystal.gbc+
    Crystal.sav onto the plain PDNA_TARGET=delta base, same one-ROM-image posture as
    run_d7_gold's own Gold-only image) -- gb_delta_pick_save()'s `if (n == 1) return 0`
    (source/pdna_main.c ~8913) means a single-ROM image skips the boot picker entirely
    and lands straight on the GB save's box grid (#279: no info page), so this needs
    exactly ONE tap (A -> box grid), not DOWN+A+A the way the combined multi-ROM image does.

    Captures: the Clock screen itself (offsets/day-count/weekday/flag readout + the
    three rows), each row's own app_confirm (Ask/Shift/Clear), the shift row's signed
    +-days/hours/minutes editor, and the shift confirm's own dynamic delta line -- B
    declines every confirm except the shift row's (which is driven through to
    gb_persist()'s PDNA_DELTA in-session-only refusal, the same honest outcome every
    other delta-build write ends at, so the shots prove the write PATH runs, not that
    it lands on a card that does not exist in this build)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b86_clock_")
    print("== BACKLOG #86/#108: the Gen-2 Clock screen (Crystal) ==")
    boot_to_gb_session(s, rom, which="crystal")
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box screen -> nav menu
    s.press_n("DOWN", 4)                                       # Party -> ... -> Clock fix (col 0, row 4)
    s.shot("01_nav_menu", "BACKLOG #86/#108: the nav menu with 'Clock fix' selected -- "
                           "NAV_OK on a Gen-2 session now (nav_avail.c's Gen-2 cell), "
                           "column 0 row 4, no RIGHT press needed")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # -> pdna_gbclock()
    s.shot("02_screen", "BACKLOG #86/#108: pdna_gbclock() itself -- the view-only "
                         "offset/day-count/weekday/flag readout at the top, then the "
                         "three rows (Ask for the time at next load / Shift the clock "
                         "/ Clear the clock-error flag), row 0 selected")

    # Row 0: "Ask for the time at next load" -> its own confirm, declined (B).
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("03_reset_confirm", "Row 1's own app_confirm -- 'Ask for the time at next "
                                "load?' / 'Asks for the time on the next CONTINUE, "
                                "like a dead battery would.'")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # decline -> back at the screen

    # Row 1: "Shift the clock" -> the signed +-days/hours/minutes editor.
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("04_shift_editor", "Row 2's own editor -- Days/Hours/Minutes steppers, all "
                               "0 on entry, L/R switches field, U/D changes it (BACKLOG "
                               "#86/#108's own signed-delta shape, never an absolute "
                               "time)")
    s.tap("UP", settle=gb_shots.SETTLE)
    s.tap("UP", settle=gb_shots.SETTLE)                       # Days field: 0 -> +2 (nonzero, so A reaches a confirm)
    s.shot("05_shift_editor_dialed", "Row 2's editor with Days dialed to +2 -- the "
                                      "field the confirm below will quote")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # a nonzero delta -> app_confirm
    s.shot("06_shift_confirm", "Row 2's own app_confirm -- 'Shift the clock by this "
                                "much?' / the exact signed delta ('+2d +0h +0m')")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # A = yes -> gbc_shift() -> gb_persist()
    s.shot("07_shift_refusal", "BACKLOG #62 D2/D5's own in-session-only refusal -- "
                                "this delta build has no SD card for gb_persist() to "
                                "write to; the shift itself already landed in RAM "
                                "(pristine re-baselined), only the write-to-card step "
                                "is refused, the same honest outcome every other "
                                "delta-build write in this tree ends at")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # dismiss -> back at the screen

    # Row 2: "Clear the clock-error flag" -> its own confirm, declined (B).
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("08_clear_confirm", "Row 3's own app_confirm -- 'Clear the clock-error "
                                "flag?' / 'Dismisses the banner; a dead battery raises "
                                "it again next boot.'")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                    # decline -> back at the screen
    return s


def run_b86_clock_gen1_fallback(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #86/#108's own Gen-1 fallback: nav_avail.c keeps NV_CLOCK's Gen-1 cell
    NAV_NOT_IN_GAME (Red/Blue/Yellow never had an RTC at all) -- gb_nav_from_start's
    generic `else if (nv != NV_BACK) app_nav_refuse(nv, kind)` branch handles it, the
    SAME "the row says not in this game" shape run_d7_gold's own NAV_COMING_SOON shot
    already proves for a different row/state. `rom` MUST be a Red-ONLY fused image
    (same single-ROM posture as run_b86_clock's own Crystal-only image, and run_d7_gold's
    Gold-only one) -- ONE tap (A) reaches the box grid, no boot picker."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b86_clock_gen1_")
    print("== BACKLOG #86/#108: the Gen-1 fallback (Red, NAV_NOT_IN_GAME) ==")
    boot_to_gb_session(s, rom, which="red")
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 4)                                       # same column-0 row 4 as the Gen-2 shot
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # -> app_nav_refuse(NV_CLOCK, SE_KIND_GEN1)
    s.shot("01_not_in_game", "BACKLOG #86/#108: Gen 1 has no clock -- app_nav_refuse() "
                              "shows nav_avail.c's own honest reason ('Gen 1 games have "
                              "no clock.'), never a dead end and never pdna_gbclock() "
                              "itself (gb_nav_from_start's NV_CLOCK branch is gated on "
                              "SE_KIND_GEN2 only)")
    return s


def run_b88_flags(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #88: pdna_gbflags.c's own Flags & counters screen -- reached via the
    standalone mount's nav menu, NV_DATA (index 8, column 0: Party->Bank->Daycare->
    Trainer->Clock fix->Mirage->Pokedex->Bag->Flags & counters, DOWN x8 from a fresh
    menu -- same col=i/PDNA_NAV_ROWS, row=i%PDNA_NAV_ROWS arithmetic run_b86_clock's
    own DOWN x4 uses for NV_CLOCK, index 4).

    `rom` MUST be a ONE-ROM fused image matching `which` ("red" -> Red.gb+Red.sav,
    "crystal" -> Crystal.gbc+Crystal.sav), same single-ROM posture as run_b86_clock/
    run_d7_gold/run_u4_bag.

    Both games share the COUNTERS tab + a toggle + CAUTION + the raw browser + B's
    confirm; they differ in which FLAGS-tab HEADER sits first (Red's own group order,
    tools/gen_gbfields.py's GROUPS_GEN1, starts with "Key events" -- an ordinary
    GBFL_KIND_TOGGLE section, so Red's own toggle/CAUTION shots come from there;
    Crystal's GROUPS_GEN2 starts with "Key items (grant in Bag)" -- a
    GBFL_KIND_BAG_GRANT section, so Crystal's own run captures the read-only bag-grant
    row instead of toggling anything in it (SELECT jumps to "Key events", the next
    section, TOGGLE-kind, for Crystal's own toggle/CAUTION shots)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b88_flags_{which}_")
    print(f"== BACKLOG #88: the Flags & counters screen ({which}) ==")
    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box screen -> nav menu
    s.press_n("DOWN", 8)                                       # Party -> ... -> Flags & counters (col 0, row 8)
    s.shot("01_nav_menu", "BACKLOG #88: the nav menu with 'Flags & counters' selected "
                           "-- NAV_OK on both kinds now (nav_avail.c's ok_both row)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # -> pdna_gbflags(), COUNTERS tab
    s.shot("02_counters_tab", "BACKLOG #88: the COUNTERS tab -- Money/Coins/Rival"
                               + ("/Safari steps" if which == "red" else "/Lucky#")
                               + ", row 0 (Money) selected")

    # A counter edit: Money's own num_entry overlay, then cancel (SELECT) so the
    # underlying value is untouched for the rest of this run.
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("03_counter_edit", "BACKLOG #88: row 0 (Money)'s own num_entry overlay -- the on-screen "
                               "keyboard/number pad, current value pre-filled")
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)              # osk_search: SELECT cancels

    # L/R swap to the FLAGS tab -- every session starts fully collapsed.
    s.tap("R", settle=gb_shots.BIG_SETTLE)
    s.shot("04_flags_tab_folded", "BACKLOG #88: the FLAGS tab, freshly entered -- every section "
                                   "FOLDED (s_gbfl_folded's own 0xFFFFFFFF starting "
                                   "state, mirrors pdna_main.c's own data editor)")

    # BACKLOG #127 P2: prove the cursor-move partial redraw runs. Every section is
    # still folded here (6 GROUPS_GEN1/GEN2 headers + the trailing raw row, all 7
    # fit in the 14-visible window with no scroll), so two plain DOWN presses move
    # the cursor across header rows 0->1->2 with `top`/the fold word both unchanged
    # -- exactly pdna_gbflags.c's `part` condition -- and touch nothing else on
    # screen. tools/gb_oracle/celldiff.py on 04_flags_tab_folded -> 04a_cursor_a and
    # 04a_cursor_a -> 04b_cursor_b must show changed cells confined to the two rows'
    # 9-px bands; the tab strip, hline and "Named flags" caption must be identical.
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("04a_cursor_a", f"BACKLOG #127 P2 ({which}): cursor moved onto header row 1 -- "
                            "partial repaint: only rows 0/1 changed")
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.shot("04b_cursor_b", f"BACKLOG #127 P2 ({which}): cursor moved onto header row 2 -- "
                            "partial repaint: only rows 1/2 changed")
    s.tap("UP", settle=gb_shots.SETTLE)
    s.tap("UP", settle=gb_shots.SETTLE)               # back to header 0 -- resumes the existing flow below

    # Unfold the first header (A on a header row folds/unfolds it).
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("05_section_opened", "BACKLOG #88: header row 0 unfolded ('+' -> '-') -- its member "
                                 "rows are now visible")

    if which == "red":
        # Red's header 0 is "Key events" (GBFL_KIND_TOGGLE) -- step onto its first
        # member row and toggle it: the one-time CAUTION, then the toggled result.
        s.tap("DOWN", settle=gb_shots.SETTLE)
        s.shot("06_flag_selected", "BACKLOG #88: the first member row of 'Key events' selected -- "
                                    "a plain GBFL_KIND_TOGGLE row")
        s.tap("A", settle=gb_shots.BIG_SETTLE)
        s.shot("07_caution", "BACKLOG #88: the one-time CAUTION ('Toggling story flags can / "
                              "soft-lock the save.') -- shown once per screen visit, "
                              "shared between the named list and the raw browser")
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # dismiss -> the toggle itself lands
        s.shot("08_toggled", "BACKLOG #88: the flag toggled ON/off -- gbfl_set wrote the bit "
                              "straight into the session's own RAM image")

        # BACKLOG #127 F3: "Hall of Fame rating" (the 33-char label that smashed 6
        # bytes past row[40] before D1's fix) lives in the LAST group, "Story"
        # (GROUPS_GEN1's own last entry, GBFL_KIND_READONLY) -- it stays folded in
        # every other Gen-1 shot in this run. SELECT jumps by row INDEX to the next
        # header regardless of fold state (same mechanic the raw-row jump below
        # already relies on). BACKLOG #129: the jump count is now DERIVED from
        # GROUPS_GEN1's own header order (tools/gen_gbfields.py, imported directly)
        # instead of a hand-copied "5" that a future group insertion could drift
        # stale -- the exact bug class BACKLOG #118's NV_GB fix already caught once.
        story_jumps = select_jumps_to(gen_gbfields.GROUPS_GEN1, "Key events", "Story")
        s.press_n("SEL", story_jumps, settle=gb_shots.SETTLE)
        # Assert the landing is REALLY "Story", not a neighbouring header a wrong
        # jump count would silently land on instead (fixed-crop pixel signature,
        # tools/gb_oracle/refs/gbflags_story_header.png -- the same technique
        # boot_to_gb_session() uses, since tools/gb_oracle/oracle.py's own tilemap
        # reader cannot attach to this GBA-hosted harness, see that helper's
        # docstring for the full reasoning).
        assert_screen(s, "gbflags_story_header")
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # unfold "Story"
        s.tap("DOWN", settle=gb_shots.SETTLE)                # -> "Beat Champion Rival"
        s.tap("DOWN", settle=gb_shots.SETTLE)                # -> "Hall of Fame rating"
        s.shot("08c_story_hof_rating", "BACKLOG #127 F3: the Story section unfolded, 'Hall "
                                        "of Fame rating' selected -- the exact 33-char label "
                                        "that smashed row[40] before D1's fix; under "
                                        "GBFL_ROW_FMT/row[64] the row reads whole, no '~'")

        jump_presses = 1   # already past h1..h5 (Story) -- one more SELECT reaches the raw row
    else:
        # Crystal's header 0 IS "Key items (grant in Bag)" -- show its read-only
        # row, then SELECT-jump to the next (TOGGLE-kind) section for the toggle/
        # CAUTION shots.
        s.tap("DOWN", settle=gb_shots.SETTLE)
        s.shot("06_bag_grant_row", "BACKLOG #88: the first member row of 'Key items (grant "
                                    "in Bag)' selected -- GBFL_KIND_BAG_GRANT, the "
                                    "'(bag)' suffix; unselected bag rows below are dim")
        s.tap("A", settle=gb_shots.BIG_SETTLE)
        s.shot("07_bag_grant_message", "BACKLOG #88: A on a bag-grant row -- 'Grant this from the "
                                        "Bag screen, not here.' (no flag is ever "
                                        "toggled by this row)")
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # dismiss
        s.tap("SEL", settle=gb_shots.BIG_SETTLE)          # jump to the next header ('Key events'), still FOLDED
        s.tap("A", settle=gb_shots.BIG_SETTLE)            # unfold it (A on a header row folds/unfolds)
        s.tap("DOWN", settle=gb_shots.SETTLE)             # onto its first member row
        s.tap("A", settle=gb_shots.BIG_SETTLE)
        s.shot("08_caution", "BACKLOG #88: 'Key events' (GBFL_KIND_TOGGLE): the one-time CAUTION "
                              "on its first real toggle")
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # dismiss -> the toggle lands
        s.shot("08b_toggled", "BACKLOG #88: the flag toggled -- gbfl_set wrote the bit; the "
                               "row's ON/off state is now visible (D3's "
                               "'%-16s %-3s %s' row format -- the old "
                               "'%-18s %s %s' shape truncated it off-screen)")
        jump_presses = 5   # from inside header 1's group: h2,h3,h4,h5,raw

    # SELECT-jump around to the trailing raw-browser row, then open it. The jump
    # walks forward through ROW INDICES (not folded-visible positions) to the next
    # header or the raw row.
    for _ in range(jump_presses):
        s.tap("SEL", settle=gb_shots.SETTLE)
    s.shot("09_raw_row_selected", "BACKLOG #88: SELECT-jumped to the trailing 'Raw flag browser "
                                   "(#N)...' row -- mirrors Gen 3's own flags_raw_view "
                                   "entry point")
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("10_raw_browser", "BACKLOG #88: the raw flag browser -- every bit of the whole event-"
                              "flags region (2560 Gen 1 / 2048 Gen 2 bits), #N "
                              "centred, ON/off per row")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # back to the FLAGS tab

    # B out of the screen entirely -> the commit confirm (dirty from the toggle above).
    s.tap("B", settle=gb_shots.BIG_SETTLE)
    s.shot("11_save_confirm", "BACKLOG #88: B with unsaved edits -- 'Save data changes?' / 'Edits "
                               "write immediately.' (app_confirm, before the one "
                               "gbs_finish()+gb_persist('gbflags') commit)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # decline: this run never actually commits
    return s


def run_b88_flags_d6(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #88 D3/D4 review, D6 evidence: the two row KINDS run_b88_flags() never
    reaches on Crystal -- GBFL_KIND_WARN's own confirm (the Kanto section, D4's fixed
    wording) and GBFL_KIND_READONLY's own message ("STORY FLAG"). `rom` MUST be a
    Crystal-only fused image (Crystal.gbc+Crystal.sav), same single-ROM posture as
    run_b88_flags's own "crystal" branch.

    GROUPS_GEN2 header order (tools/gen_gbfields.py): 0 Key items (grant in Bag),
    1 Key events, 2 Gym Leaders, 3 Elite Four, 4 Story (READONLY), 5 Kanto
    (post-game) (WARN) -- SELECT from header 0 steps forward one header per press."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b88_flags_d6_")
    print("== BACKLOG #88 D6: WARN confirm + READONLY message (crystal) ==")
    boot_to_gb_session(s, rom, which="crystal")
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 8)                                     # -> Flags & counters row
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # -> pdna_gbflags(), COUNTERS tab
    s.tap("R", settle=gb_shots.BIG_SETTLE)                   # -> FLAGS tab, all folded, header 0 selected

    # SELECT x4 -> header 4 "Story" (READONLY), unfold it, step onto its first member row.
    for _ in range(4):
        s.tap("SEL", settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # unfold "Story"
    s.tap("DOWN", settle=gb_shots.SETTLE)                    # onto its first member row
    s.shot("01_story_row_selected", "BACKLOG #88: D6 -- 'Story' section unfolded, its first "
                                     "GBFL_KIND_READONLY row selected -- the '(sty)' "
                                     "suffix (D3's shortened kind_suffix)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("02_story_flag_message", "BACKLOG #88: D6 -- A on a READONLY row -- 'STORY FLAG' / "
                                     "'This is a display-only / progress flag.' "
                                     "(msg_wait, no flag touched)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # dismiss

    # SELECT x1 more -> header 5 "Kanto (post-game)" (WARN), unfold, step onto its one row.
    # NOTE: Guy's own Crystal.sav corpus already has EVENT_RESTORED_POWER_TO_KANTO ON
    # (a progressed save, not fresh) -- the first toggle attempted below is therefore
    # the ON->OFF direction, not OFF->ON; captions below describe what the emulator
    # actually showed, not the direction originally assumed when this flow was written.
    s.tap("SEL", settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # unfold "Kanto (post-game)"
    s.tap("DOWN", settle=gb_shots.SETTLE)                    # onto EVENT_RESTORED_POWER_TO_KANTO
    s.shot("03_kanto_row_selected", "BACKLOG #88: D6 -- 'Kanto (post-game)' unfolded, its one "
                                     "GBFL_KIND_WARN row selected -- ON on this corpus "
                                     "save (already progressed past this event)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # CAUTION fires first (D4: reordered ahead of the WARN confirm)
    s.shot("04_caution_before_warn", "BACKLOG #88: D6 (D4 fix) -- the generic CAUTION now fires "
                                      "BEFORE the WARN-specific confirm on the very "
                                      "first toggle of a WARN row")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # dismiss CAUTION -> the WARN confirm itself
    s.shot("05_warn_confirm_remove", "BACKLOG #88: D6 (D4 fix) -- the WARN confirm's ON->OFF "
                                      "direction (row starts ON here) -- 'Remove Kanto "
                                      "power?' / 'Kanto becomes unreachable.' (used to "
                                      "show the SAME 'Restore power' text on this path "
                                      "before D4)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # confirm removal -> flag now OFF
    s.shot("06_kanto_off", "BACKLOG #88: D6 -- confirmed -- the row now reads off")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # toggle again: now OFF, going ON
    s.shot("07_warn_confirm_restore", "BACKLOG #88: D6 (D4 fix) -- the WARN confirm's OTHER "
                                       "direction, OFF->ON -- 'Restore power to Kanto?' "
                                       "/ 'Lets Kanto be reached early.' (the ORIGINAL, "
                                       "still-correct wording for this direction)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # decline: leave Kanto off (as toggled above), don't commit
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # back out to the commit confirm
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # decline: this run never actually commits
    return s


# ---------------------------------------------------------------------------------
# BACKLOG #87: the shared Pokedex screen on Gen 1/2.
# ---------------------------------------------------------------------------------

def run_b87_dex(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #87: pdna_gbdex()'s Pokedex screen (item 3) -- reached via the standalone
    mount's nav menu, NV_DEX (index 6, column 0: Party->Bank->Daycare->Trainer->Clock
    fix->Mirage->Pokedex, DOWN x6 from a fresh menu, same PDNA_NAV_ROWS==10 arithmetic
    run_b86_clock's own docstring explains -- index 6 stays in column 0, no RIGHT).

    `rom` MUST be a ONE-ROM fused image (tools/fuse_gb.py, same posture as every other
    single-ROM shot function here): Red.gb + a Red.sav that has had `--op dexset 100
    0` applied first for `which="red"`, or Crystal.gbc + a Crystal.sav that has had
    the SAME `--op dexset 100 0` applied first for `which="crystal"` (this file's own
    corpus is a fully-completed dex -- host_gbdex_test.c's own popcount proof already
    established that -- so dex #100 is pre-cleared on BOTH games here purely so the
    'A cycles one cell through seen/caught/none' shot has a real none->seen->caught
    transition to show, not a caught->none->seen one; item 6's own retail-gate proves
    the write path against real WRAM, this is only a visual demo). N4 (b87 fix pass,
    DO-NOT-SHIP review): the cycle demo below moves the cursor to dex #100 itself
    (99x DOWN from the list top) so the pre-clear actually applies to the cell being
    cycled -- an earlier pass pre-cleared #100 but cycled whatever cell the cursor
    started on (species #1, Bulbasaur, list top), so the pre-clear never showed up on
    screen.

    Red (Gen 1, no Unown): pdna_gbdex() opens the shared pdna_dex_screen() directly,
    no chooser. Crystal (Gen 2): pdna_gbdex() shows its own entry chooser first
    ("Pokedex" / "Unown forms") -- BOTH sub-screens get their own shots here.

    Captures (per item 7): the grid at 1:1 (species cap visible -- Red stops at #151,
    Crystal at #251, no row past it), the list view (L), A cycling dex #100 through
    none/seen/caught, the START menu (dex_menu -- filter/sort/status/"Mark all..."),
    Catch ALL + Undo (dex_bulk, the "Mark all..." row), B -> the shared screen's own
    exit, on Crystal only: the Unown forms 26-row toggle list from the chooser's
    second row, B -> confirm ("Save Pokedex changes?")."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    tag = f"b87_dex_{which}_"
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, tag)
    print(f"== BACKLOG #87: the Pokedex screen ({which}) ==")
    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box screen -> nav menu
    s.press_n("DOWN", 6)                                       # Party -> ... -> Pokedex (col 0, row 6)
    s.shot("01_nav_menu", "#87: the nav menu with 'Pokedex' selected -- "
                           "NAV_OK on both generations now (nav_avail.c's GB_TABLE), "
                           "column 0 row 6, no RIGHT press needed")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # -> pdna_gbdex()

    if which == "crystal":
        s.shot("02_chooser", "#87: Gen 2's own entry chooser -- "
                              "'Pokedex' / 'Unown forms', Pokedex row selected")
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # chooser row 0 -> pdna_dex_screen()

    # ARTLESS DEFAULT IS LIST, not grid: pdna_dex_screen's own `view = mon_icon_for(1)
    # ? DV_GRID : DV_LIST` (pdna_pick.c) -- mon_icon_for(1) is NULL in this (art-free,
    # delta-artless) shot vehicle, so the FIRST screen this session sees is DV_LIST.
    s.shot("03_list_default", "#87: The shared pdna_dex_screen(), DV_LIST (this artless "
                               "shot vehicle's own default when no icon art is "
                               "compiled -- pdna_pick.c's `view = mon_icon_for(1) ? "
                               "DV_GRID : DV_LIST`), item 1's species cap applied "
                               f"({'151' if which == 'red' else '251'} species, no "
                               "row past it, S/C counts in the header) -- "
                               "pdna_pick.c UNCHANGED, same screen Gen 3 uses")

    # BACKLOG #253: L's own BIG_SETTLE (40 frames) is not long enough for this page's
    # 21-cell GB-ROM icon repaint to finish -- reproduced live: a capture taken right
    # here (before the extra s.run() below) shows only 8 of 21 cells and NO header
    # text yet, and the very next R tap's key-down edge lands inside that still-busy
    # repaint and is silently dropped (200 more idle frames afterward, with no further
    # input, still shows DV_GRID -- not a render lag, a genuinely missed edge). The
    # symptom this produced downstream: R never took the screen back to DV_LIST, so
    # the 99x DOWN below (whose "row i == dex i+1" math is LIST-only -- DV_GRID's own
    # KEY_DOWN handler advances `sel` by `cols` (7) per press, not 1) walked a 2-D grid
    # instead and landed on dex #85 (DODRIO), not #100, contradicting every caption
    # from here through 10_after_catch_all. Extra settle here, before the R tap fires,
    # is the fix: R has never once dropped an edge once this page was allowed to finish
    # loading first (reproduced clean on 5 separate runs).
    s.tap("L", settle=gb_shots.BIG_SETTLE)                   # DV_LIST(1) -> DV_GRID(0)
    s.run(GB196_GB_PAGE_SETTLE)                              # same repaint weight as run_b124_dexicons's 21-cell GB-ROM page (dex_cell_art_call, pdna_pick.c:802) -- reuse its measured constant, not a new guessed one
    s.shot("04_grid", "#87: L once -> DV_GRID -- BACKLOG #196's GB-ROM-native icon "
                       "override (dex_cell_art_call, pdna_pick.c:802): Red's own "
                       "DMG-grey front sprites, not the art-free name-chip fallback "
                       "-- same cap")

    s.tap("R", settle=gb_shots.BIG_SETTLE)                   # DV_GRID(0) -> DV_LIST(1), back where item 5/6's cursor math below assumes

    # N4 (b87 fix pass, DO-NOT-SHIP review): move the cursor to dex #100 itself (99x
    # DOWN from the list top, filter=All/sort=dex-number so the list is in plain
    # national-dex order with no gaps -- row i == dex i+1) so the pre-clear this
    # function's own caller applied (`--op dexset 100 0`) is the cell actually being
    # cycled below, not species #1 (Bulbasaur, list top) as an earlier pass did.
    s.press_n("DOWN", 99)
    s.shot("04b_cursor_at_100", "#87 N4: cursor moved to dex #100 (99x DOWN, list "
                                 "order == national-dex order) -- 'none' (pre-cleared "
                                 "by --op dexset 100 0) before the cycle demo below")

    # A cycles the selected cell through none/seen/caught (dex_state's own state
    # 0/1/2 = none/seen/caught cycle, pdna_pick.c's `(s_dget(nat)+1) % 3`). Dex #100
    # was pre-cleared to 'none' by the caller's `--op dexset 100 0`, so this cell
    # (not species #1) is the one that genuinely shows none->seen->caught.
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("05_cycle_a", "#87: A pressed once on dex #100 -- state advanced one step "
                          "(dex_state's own 0/1/2 -> +1 mod 3 cycle; starts 'none' "
                          "per the pre-clear, so this step lands on 'seen')")
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("06_cycle_b", "#87: A pressed a second time -- one more step around the "
                          "cycle ('seen' -> 'caught')")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # back to 'none' -- leave the cell as the pre-clear found it

    s.tap("START", settle=gb_shots.BIG_SETTLE)               # -> dex_menu()
    s.shot("07_start_menu", "#87: KEY_START -> dex_menu() -- sort/status toggles + "
                             "'Mark all...' (can_edit) + the filter list. Same screen "
                             "as Gen 3, but D5's cap gate hides the rows that would "
                             "page to nothing here: no 'Gen 2'/'Gen 3' on a Gen-1 "
                             "session, no 'Gen 3' on a Gen-2 one")

    s.press_n("DOWN", 2)                                        # row 0 sort, row 1 status, row 2 "Mark all..."
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # -> dex_bulk() overlay ("stay open": dex_menu itself does not close)
    s.shot("08_bulk_menu", "#87: dex_bulk()'s own overlay, opened from dex_menu's "
                            "'Mark all...' row -- Catch/See/Wipe ALL, the "
                            "National-Dex toggle row HIDDEN (setnat is NULL on GB, "
                            "item 3's own contract), Undo not yet available")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # Catch ALL -> its own app_confirm
    s.shot("09_catch_all_confirm", "#87: Catch ALL's own app_confirm -- the bulk-confirm "
                                    "text now reads the LIVE cap ('All 151.'/'All "
                                    "251.'), item 1's siprintf fix, not a hardcoded "
                                    "386")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # confirm -> every species caught, snapshot taken -- dex_menu's own loop is still open (dex_bulk's "stay open" contract)
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # dex_menu -> B closes it, back to pdna_dex_screen's own grid/list
    s.shot("10_after_catch_all", "#87: back at the list after Catch ALL -- every cell "
                                  "now shows CAUGHT")

    s.tap("START", settle=gb_shots.BIG_SETTLE)               # dex_menu reopens fresh, cursor back at row 0
    s.press_n("DOWN", 2)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # -> dex_bulk() again
    s.shot("11_undo_available", "#87: dex_bulk() reopened -- 'Undo last' now offered "
                                 "(s_dex_snap_valid from the Catch ALL above), no "
                                 "National-Dex row (Catch/See/Wipe ALL, Undo last, "
                                 "Cancel -- 5 rows, not 6: setnat is NULL on GB)")

    # Undo is row index 3 (Catch=0, See=1, Wipe=2, Undo=3, Cancel=4 -- no National-Dex
    # row on GB, setnat NULL) -- 3 DOWN presses from the overlay's own default sel=0.
    s.press_n("DOWN", 3)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # Undo -> restores the pre-Catch-ALL snapshot; dex_menu's own loop is STILL open
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # dex_menu closes
    # BACKLOG #251 shots-refresh re-derivation (was: "back at the list after Undo" for
    # BOTH games, then one more B -> the save confirm for both): reproduced live,
    # three times, on Red only -- this SAME dex_menu-closing B, right after Undo (not
    # after Catch ALL two shots earlier, which correctly lands on the list, see
    # 10_after_catch_all above), lands directly on pdna_gbdex()'s own "Save Pokedex
    # changes?" confirm for Red, skipping the list screen entirely; Crystal's own
    # capture (b87_dex_crystal_12_after_undo.png) DOES still show the list, matching
    # the original caption. Not chased to a root cause in source/pdna_gbdex.c (out of
    # this lane's scope) -- shooting what is actually on screen for each game, and
    # correcting Red's downstream taps to match (see the which=="red" branch below):
    # the old code's extra "B" after this point CANCELLED this already-showing
    # confirm (B = no) and cascaded out to the Bank/box screen, so 16_save_confirm was
    # silently capturing a totally unrelated screen (a Bank grid) under a "Save
    # Pokedex changes?" caption on every Red run.
    if which == "crystal":
        s.shot("12_after_undo", "#87: back at the list after Undo -- restored to exactly "
                                 "the pre-Catch-ALL state (dex_bulk's s_dex_snap[386], "
                                 "byte-exact per the acceptance gate) -- compare against "
                                 "03_list_default")
        s.tap("B", settle=gb_shots.BIG_SETTLE)               # dex screen -> back to the chooser
        s.tap("DOWN", settle=gb_shots.SETTLE)                # chooser row 1: Unown forms
        s.shot("13_chooser_unown_row", "#87: the chooser with 'Unown forms' selected")
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # -> unown_forms_screen()
        s.shot("14_unown_list", "#87: the 26-row Unown A..Z toggle list -- item 3's own "
                                 "plain-list idiom over gb_dex.h's wUnownDex core")
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # toggle letter A
        s.shot("15_unown_toggled", "#87: letter A toggled -- trainer_flag_row_paint's own "
                                    "ON/off text flips")
        s.tap("B", settle=gb_shots.BIG_SETTLE)               # Unown list -> back to the chooser
        s.tap("B", settle=gb_shots.BIG_SETTLE)               # chooser -> pdna_gbdex()'s own confirm
        s.shot("16_save_confirm", "#87: 'Save Pokedex changes?' -- item 3's own end-of-visit "
                                   "confirm (gbs_finish + gb_persist('dex') on A)")
    else:
        # BACKLOG #253 re-derivation: e185112 (shots-refresh) reported that this SAME
        # dex_menu-closing B, on Red only, closes dex_menu AND pdna_dex_screen() in one
        # press, skipping straight to pdna_gbdex()'s confirm. Source says that cannot
        # happen: dex_menu() (pdna_pick.c ~949) is its OWN `for(;;)` loop with its own
        # `k & KEY_B` handling (`return bulked?2:(changed?1:0);`) -- a single B keypress
        # is consumed by exactly one s_wait() call, either dex_menu's or the outer
        # pdna_dex_screen() loop's (`if (k & KEY_B) break;`), never both. #253's own fix
        # above (L then R was landing on the wrong DV_* index because L's own settle was
        # too short for the R tap that followed) is the far more likely explanation for
        # e185112's capture: reproduced clean, twice, deterministically, against the
        # FIXED chain -- one B here lands on the list, exactly like Crystal's own
        # 12_after_undo, not the confirm. Red has no chooser to return through first
        # (Gen 1 skips gbdex_chooser() entirely, pdna_gbdex.c's own `s->gen == GB_GEN2`
        # branch), so ONE more B from here (not Crystal's B -> chooser -> B) is enough
        # to exit pdna_dex_screen() itself and reach the confirm.
        s.shot("12_after_undo", "#87: back at the list after Undo -- restored to exactly "
                                 "the pre-Catch-ALL state (dex_bulk's s_dex_snap[386], "
                                 "byte-exact per the acceptance gate) -- compare against "
                                 "03_list_default; this closing B only exits dex_menu's "
                                 "own nested loop, same as Crystal's 12_after_undo")
        s.tap("B", settle=gb_shots.BIG_SETTLE)               # dex screen's own B -> exits pdna_dex_screen() (Red has no chooser in between) -> pdna_gbdex()'s confirm
        s.shot("15_save_confirm", "#87: 'Save Pokedex changes?' -- item 3's own "
                                   "end-of-visit confirm (gbs_finish + "
                                   "gb_persist('dex') on A) -- Red reaches it one B "
                                   "sooner than Crystal (no chooser layer to return "
                                   "through first)")
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # confirm (yes) -> gb_persist('dex')
        s.shot("16_save_confirm", "#87: confirmed -> refused: \"GAME BOY SAVE / Edits "
                                   "are in-session only in the emulator build.\" -- the "
                                   "same honest SD-write refusal every other GB commit "
                                   "path in this file shows (gb_persist()'s PDNA_DELTA "
                                   "branch); no flashcart in mGBA")
    return s


# ---------------------------------------------------------------------------------
# BACKLOG #124: the Gen-1/2 Pokedex grid draws GB ROM icons, like the GB box grid.
# ---------------------------------------------------------------------------------
def run_b124_dexicons(core_mod, image_mod, rom: Path, out_dir: Path, which: str,
                      fallback: bool = False) -> gb_shots.Session:
    """BACKLOG #124: pdna_pick.c's dex_cell_grid() now calls through pdna_gbdex.c's
    gbdex_cell_art() override for a Gen-2 GB session, drawing the SAME GB ROM icon
    rendition pdna_box.c's box grid already shows for a Gen-2-resolved cell (both
    reach pdna_origin_art.c's fetch_icon() -- see pdna_origin_art_icon()'s own
    header comment).

    `rom` must be a single-ROM fused image (tools/fuse_gb.py, same posture as
    run_b87_dex): Crystal.gbc + Crystal.sav for `which="crystal"` (Gen 1 has no menu
    icons at all -- gb_art_source.c's own rule -- so `which="red"` is box-grid-only,
    no dex-icon claim to make; this function still opens Red's dex screen once, to
    show the UNCHANGED icon-store list/grid a Gen-1 session keeps).

    `fallback=True` expects `rom` to carry the save but NO Crystal.gbc (fuse_gb.py
    with just the .sav) -- gbdex_cell_art()'s pdna_origin_art_have(PDNA_GEN2) refuses,
    so dex_cell_grid() falls straight back to the icon-store/mon_icon_for ladder
    exactly as before this backlog, same shot sequence, different expected pixels.

    Captures: the box grid (pdna_box.c's existing GB-icon cell, for a same-species
    side-by-side comparison), the dex grid page 1 (this backlog's new render, or the
    icon-store fallback when `fallback`), and (crystal, non-fallback only) the SAME
    dex page's `perf dex:` log line so step 2's SD-read count can be read back off
    it (see measure_dex_sd_reads() below, which reuses this exact navigation)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255 review: its own inline comment turns on mon_icon_for(1) being NULL -- the exact
                                        # icon-lookup divergence that made BACKLOG #253 fourteen wrong frames
    tag = f"b124_dexicons_{which}{'_fallback' if fallback else ''}_"
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, tag)
    print(f"== BACKLOG #124: dex-grid GB icons ({which}{' fallback' if fallback else ''}) ==")
    s.run(700)
    # #279: single-ROM image lands on the grid by itself (no S1 info page, no tap)
    s.run(GB_ART_COLD_SETTLE)
    s.shot("01_box_grid", "#124: the box grid BEFORE the dex visit -- pdna_box.c's "
                           "own GB-icon cell (pdna_origin_box_art(), unrelated to "
                           "this backlog), for a same-species side-by-side against "
                           "the dex grid shot below")
    s.tap("START", settle=gb_shots.BIG_SETTLE)               # box screen -> nav menu
    s.press_n("DOWN", 6)                                       # Party -> ... -> Pokedex (col 0, row 6)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # -> pdna_gbdex()

    if which == "crystal":
        s.tap("A", settle=gb_shots.BIG_SETTLE)               # chooser row 0 (Pokedex) -> pdna_dex_screen()

    # ARTLESS DEFAULT IS LIST (mon_icon_for(1) is NULL in this shot vehicle) --
    # L once -> DV_GRID, the view dex_cell_grid()'s override actually paints.
    # BACKLOG #196: a GB-served page (a ROM is present, `not fallback`) does 21 REAL
    # per-cell ROM fetches on this one repaint -- GB196_GB_PAGE_SETTLE, not
    # BIG_SETTLE, or "02_dex_grid" (and bobcheck's own frame A, which reuses this
    # exact session state with no further settle of its own) can be captured
    # mid-paint. Applied to BOTH games (see the constant's own comment for why).
    settle = GB196_GB_PAGE_SETTLE if not fallback else gb_shots.BIG_SETTLE
    s.tap("L", settle=settle)
    s.shot("02_dex_grid", "#124/#196: DV_GRID page 1 -- " +
           ("GB ROM icons via gbdex_cell_art() for every seen/caught species (this "
            "backlog's own render)" if (which == "crystal" and not fallback) else
            "BACKLOG #196: the ROM's own Gen-1 front sprites (4 DMG greys), via "
            "pdna_origin_art_portrait_by_dex() -- Red no longer falls to the "
            "icon-store/name-chip ladder" if (which == "red" and not fallback) else
            "no GB ROM registered (fallback image) -- pdna_origin_art_have() "
            "refuses, dex_cell_grid() falls straight back to the unchanged icon-store "
            "ladder, byte-identical to a pre-#124/#196 build"))
    return s


def run_b124_bobcheck(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> None:
    """BACKLOG #124 review A5 follow-up: proves the caught-cell bob-animation flip
    still runs on a session dex_cell_art_serves_page() answers false for (a Gen-1
    session, even with a Gen-2 ROM ALSO registered in the same fused image -- the
    exact bug this fix targets), while a session it answers true for (Crystal, same
    image) keeps showing GB icons. Reuses run_b124_dexicons()'s own navigation to
    reach the dex grid, then captures a burst of frames (45 frames total, sampled every
    5th frame = 9 samples, 1.5 animation periods) and diffs each against the first frame
    with PIL. BACKLOG #137: stride-5 sampling avoids the aliasing trap of the original
    stride-30 approach (equal to DEX_ANIM_PERIOD itself), which could land both frames
    on the same animation phase despite a 4-6 frame redraw wave. Prints the max diff
    across all samples; a genuinely bobbing page has a NON-ZERO max diff, a static page
    (pre-fix behaviour on the Gen-1+Gen-2-ROM image) has ZERO."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    from PIL import Image, ImageChops
    s = run_b124_dexicons(core_mod, image_mod, rom, out_dir, which, fallback=False)
    # s is now sitting on the dex grid (DV_GRID, page 1) right after run_b124_dexicons's
    # own "02_dex_grid" shot -- no further navigation needed.
    p1 = s.shot("03_bob_a", "#124 review A5: bob-check frame A (burst start)", allow_same=True)
    im1 = Image.open(p1).convert("RGB")

    # BACKLOG #137: capture burst at stride 5 (not stride 30=DEX_ANIM_PERIOD) to avoid
    # aliasing against the animation period. The 21-cell redraw takes 4-6 real frames;
    # stride 30 can land both samples on the same animation phase. Stride 5 ensures we
    # cross multiple phases within 45 frames (1.5 periods).
    max_nonzero = 0
    frames_data = []
    for i in range(9):  # 9 samples * 5 frames = 45 frames total (1.5 * DEX_ANIM_PERIOD)
        s.run(5)
        pi = s.shot(f"03_bob_s{i+1}", f"#124 review A5: bob-check sample {i+1}/9", allow_same=True)
        frames_data.append(pi)
        imi = Image.open(pi).convert("RGB")
        diff = ImageChops.difference(im1, imi)
        # A caught cell in the grid's top-left region. From dex_geom() (pdna_pick.c):
        # y0=24, ch=34 (cell height), vrows=3 (visible rows). Crop box covers all 3 rows
        # with margin: (0, 20, 240, y0 + vrows*ch + margin) = (0, 20, 240, 24+3*34+4) = (0, 20, 240, 130).
        box = (0, 20, 240, 130)
        region_diff = diff.crop(box)
        nonzero = sum(1 for px in region_diff.getdata() if px != (0, 0, 0))
        max_nonzero = max(max_nonzero, nonzero)

    print(f"== BACKLOG #124 review A5 bob-check ({which}) ==")
    print(f"  frame A (baseline): {p1}")
    print(f"  burst samples 1-9: {frames_data[0]} ... {frames_data[-1]}")
    print(f"  max non-zero pixels in the top grid rows' diff (stride-5 burst): {max_nonzero} "
          f"({'BOBBING (differs)' if max_nonzero else 'STATIC (identical) -- would be the bug if this session should serve GB'})")


def measure_dex_sd_reads(core_mod, image_mod, rom: Path, out_dir: Path, which: str = "crystal") -> None:
    """BACKLOG #124 step 2 (BACKLOG #196: `which` generalised from a Crystal-only
    hardcode so the SAME measurement can run against a single-ROM Red image too --
    Red's dex grid drew NOTHING from the GB rung before #196 (Gen 1 self-refused and
    fell to the icon-store ladder), so a `which="red"` run before #196 is the "before"
    half of that feature's own report; after #196 it exercises the new Gen-1 front-
    pic rung). The 'perf dex:' log line's own `sd Xr/...` field for ONE dex-grid page
    (run_b124_dexicons's exact navigation, GB ROM registered, no fallback) against
    the SAME image's box-grid 'perf box:'/'perf bank:' line for a comparable
    single-page paint -- both spans already exist in this tree (pdna_pick.c's
    pdna_dex_screen() and pdna_box.c's own span), so this reuses them rather than
    adding new instrumentation. Installs a capturing logger (same mechanism as
    tools/perf_parity.py's load_mgba_capturing) instead of taking screenshots, and
    just prints every 'perf ' line captured during the run -- read the LAST
    'perf dex: ... sd Nr/...' and 'perf box:'/'perf bank: ... sd Nr/...' lines by
    hand off stdout (no parser here: this is a one-off measurement, not a gate this
    tool enforces).

    HARDWARE-ONLY CAVEAT (BACKLOG #196 report): this `--image` is always a `make
    delta-artless` + fuse_gb.py single-ROM fuse, i.e. the PDNA_DELTA build variant --
    gb_art_source.c's PDNA_DELTA half reads the GB ROM straight out of cart address
    space via fused_gb_rom() (fused_gb.h), NEVER through FatFs/`f_open`/disk_read, so
    `perf_sd.rd` (the counter 'perf dex: sd Nr' actually reads) is architecturally
    fixed at 0 on this vehicle no matter what the dex screen does -- the emulator
    cannot exercise the real SD path at all (no SD card is emulated; the ONLY
    non-PDNA_DELTA build is the hardware SD-mode build, `make`/`make sd`, which this
    harness cannot run). The real per-page SD-read cost this feature actually changes
    is therefore a hardware-only number -- see the BACKLOG #196 report's own fetch-
    CALL-COUNT proxy (which maps 1:1 to f_open count on the real SD build, per
    gb_art_source.h's own header comment) for the provable, non-hardware-only
    evidence that this feature reduces I/O."""
    lines: list[str] = []
    log_mod = getattr(core_mod, "log", None)
    if log_mod is None:
        import mgba.log as log_mod  # noqa: E402 (matches perf_parity.py's own import shape)

    class Capture(log_mod.Logger):
        def log(self, category, level, message):  # noqa: A002
            try:
                m = log_mod.ffi.string(message).decode("utf-8", "replace")
            except TypeError:
                m = str(message)
            lines.append(m)

    log_mod.install_default(Capture())
    s = run_b124_dexicons(core_mod, image_mod, rom, out_dir, which, fallback=False)
    s.run(150)   # let perf's rate-limited flush land (perf_parity.py's own FLUSH_SETTLE)
    print(f"== BACKLOG #124/#196 step 2: captured perf lines ({which}) -- read 'perf dex:'/"
          "'perf box:'/'perf bank:' sd Nr by hand ==")
    for l in lines:
        if l.startswith("perf "):
            print(f"  {l}")


def run_b208_dexcache(core_mod, image_mod, rom: Path, out_dir: Path, which: str = "crystal") -> None:
    """BACKLOG #208 step 3: proves the real per-page GB-art cache's HIT/MISS shape
    by capturing every 'dexart: page N fetch=X hit=Y' log line (source/pdna_gbdex.c's
    PDNA_DELTA-only page-begin hook, pdna_pick.h's PdnaDexPageFn) across a fixed
    navigation script, the same log-capturing technique measure_dex_sd_reads() above
    already uses (Capture installed as mgba's default logger BEFORE the session
    boots). `rom` must be a ONE-ROM fused delta-artless image (fuse_gb.py), same
    posture as --b124-dexicons.

    BACKLOG #208 fixes review D2: gbdex_dex_page_begin() (pdna_gbdex.c) prints a
    page's tally at the START of the NEXT page, not at the end of its own -- every
    line `dump()` prints below therefore belongs to the phase BEFORE the one it is
    printed under, never the phase named in the dump() call that surfaces it. Read
    each printed line as "the tally of the page number IT names, flushed now" -- not
    as evidence about the step that triggered the print.

    `run_b124_dexicons()`'s own navigation lands on the dex grid's page 1 with L
    already pressed (DV_GRID) -- that first full repaint is this bench's own "cold
    entry" page (page 1: 21 fetch / 0 hit expected, the cache starts empty every
    visit; nothing prints yet -- there is no earlier page to flush).

      same-page repaint -- START opens dex_menu's filter overlay, B cancels it with
        NEITHER the view nor top changed (pdna_pick.c's own `gen != ui_clear_gen()`
        term is what forces the next `full` repaint here, not a real navigation) --
        this step's own dexart: line is page 1's flush: fetch=21 hit=0 (the COLD
        ENTRY tally above, only visible now).

      cursor move (no scroll) -- RIGHT then LEFT moves the selection within the
        page without crossing a row edge -- dex_declare_page() is only called on a
        FULL repaint (a fresh page_begin), so this step is expected to add NO new
        'dexart:' line at all (0 fetches, trivially, because nothing re-declares a
        page) -- this is BACKLOG #208's own "open a cell then return -> 0" case,
        read literally: no per-cell detail screen exists yet (BACKLOG #203), so the
        closest equivalent this build has is moving onto a cell and back.

      row scroll -- DOWN x3 moves the cursor down one full row (cols=7 in the grid
        view), crossing the bottom edge and forcing a one-row scroll -- this step's
        own dexart: line is page 2's flush: fetch=0 hit=21 (the SAME-PAGE REPAINT
        tally). The scroll's OWN tally (fetch=7 hit=14: 7 cells scrolled off the top
        get evicted and refetched, the other 14 still on screen stay cached) only
        shows up at the NEXT full repaint this bench does not trigger -- 5 is this
        cache's own FIFO eviction count for that scroll (tests/host_dexgbartcache_test.c
        part E), not the fetch count; do not conflate the two.

    A "page flip" (scroll a WHOLE further page, 21 cells, off screen) is not
    exercised as a separate step here: this cache's own page (vis) is exactly 21
    cells and its capacity is 23 slots -- 21 <= 23 unconditionally caps a single
    repaint's fetch count at 21 (there are never more than 21 cells on screen to
    fetch), so "page flip -> 21" is a ceiling this bench's cold-entry step (also 21)
    already demonstrates, not a distinct scenario worth a fourth navigation phase.

    Captures three frames (BACKLOG #208 fixes review D2) at the SAME points the
    fetch/hit counts above are being reasoned about: the cold entry, the same-page
    repaint, and the row scroll -- for a byte-compare against main's frames from the
    same fused inputs.

    Prints every captured 'dexart:' line, per phase, for a human (or a future
    machine parser -- BACKLOG #184's own aspiration) to read the counts off -- same
    "no parser here" posture as measure_dex_sd_reads() above."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    lines: list[str] = []
    log_mod = getattr(core_mod, "log", None)
    if log_mod is None:
        import mgba.log as log_mod  # noqa: E402

    class Capture(log_mod.Logger):
        def log(self, category, level, message):  # noqa: A002
            try:
                m = log_mod.ffi.string(message).decode("utf-8", "replace")
            except TypeError:
                m = str(message)
            lines.append(m)

    log_mod.install_default(Capture())
    s = run_b124_dexicons(core_mod, image_mod, rom, out_dir, which, fallback=False)
    s.run(150)

    def dump(phase: str, mark: int) -> int:
        new = [l for l in lines[mark:] if l.startswith("dexart:")]
        print(f"  -- {phase} --")
        for l in new:
            print(f"    {l}   (tally of the page named in the line, flushed now -- "
                  f"belongs to the PREVIOUS phase, not '{phase}')")
        if not new:
            print("    (no dexart: line -- expected iff this step never triggers a full repaint)")
        return len(lines)

    print(f"== BACKLOG #208 step 3: dexcache HIT/MISS ({which}) ==")
    mark = dump("cold entry (page 1)", 0)
    s.shot("03_b208_cold", "#208 fixes D2: cold entry, page 1 -- 21 fetch / 0 hit "
           "(this frame's own tally has not been flushed yet -- it prints at the "
           "next full repaint, see the dexart: line under the NEXT phase below); "
           "pixel-identical to run_b124_dexicons()'s own 02_dex_grid by design -- "
           "no navigation happened between them, this is the SAME state re-captured "
           "as this bench's own frame 1", allow_same=True)

    s.tap("START", settle=gb_shots.BIG_SETTLE)   # dex_menu's own filter overlay
    s.tap("B", settle=gb_shots.BIG_SETTLE)       # cancel -- same page, forces a repaint
    mark = dump("same-page repaint (START then B)", mark)
    s.shot("04_b208_repaint", "#208 fixes D2: same-page repaint (START then B) -- "
           "the frame itself is unchanged pixels (every cell re-served from cache); "
           "the dexart: line just printed above it is COLD ENTRY's own flushed "
           "tally (fetch=21 hit=0), not this step's", allow_same=True)

    s.tap("RIGHT", settle=gb_shots.SETTLE)
    s.tap("LEFT", settle=gb_shots.SETTLE)
    mark = dump("cursor move, no scroll (RIGHT then LEFT)", mark)

    s.press_n("DOWN", 3, settle=gb_shots.SETTLE)  # one full row, cols=7
    mark = dump("row scroll (DOWN x3)", mark)
    s.shot("05_b208_scroll", "#208 fixes D2: one-row scroll (DOWN x3) -- 7 of the "
           "21 visible cells are newly fetched, 14 stay cached; the dexart: line "
           "just printed above it is the SAME-PAGE REPAINT's own flushed tally "
           "(fetch=0 hit=21), not this step's")

    # One more forced repaint (same START/B trick as the "same-page repaint" step
    # above) purely to FLUSH the row scroll's own tally -- gbdex_dex_page_begin()
    # never prints a page's numbers until the NEXT page begins, so without this
    # the scroll's real fetch=7/hit=14 count would only ever be asserted in prose,
    # never actually observed from the bench's own output.
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.tap("B", settle=gb_shots.BIG_SETTLE)
    dump("flush (START then B) -- surfaces the row scroll's own tally", mark)


# ---------------------------------------------------------------------------------
# BACKLOG #104 R1: KEEP AS IS / MAKE LEGAL on a Gen 3 -> Game Boy paste.
# ---------------------------------------------------------------------------------

def run_r1_xfer(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #104 R1 (docs/TRANSFER-ROUNDTRIP-DESIGN.md section 3c/4): the KEEP AS
    IS / MAKE LEGAL choice gb_paste_hook now offers between the existing loss screen
    and the box-writable check, for a paste whose species is standing below its
    evolution's minimum level.

    `rom` must be `pokedna-delta-artless.gba` fused with THREE payloads, in this
    order (tools/fuse_sav.py then tools/fuse_gb.py, --check confirms all four
    directory entries land): an ordinary Gen-3 .sav (any works; its own contents are
    never read by this flow -- it only exists so the boot picker has a row 0 to skip
    past), an 80-byte raw Gen-3 box record via `fuse_sav.py --clip` (screenshot-only
    hook, docs already ship: "so PASTE (GB) on an empty cell reachable without a
    Gen-3 session ever having been open") built with a REAL species standing below
    its own evolution floor (Charizard, species 6, at level 20 -- pk_evo_min_level(6)
    == 36, walking Charmander -> L16 Charmeleon -> L36 Charizard), and ONE Game Boy
    ROM+save via `fuse_gb.py` ("one ROM per image" per the R1 brief).

    GOLD, NOT RED -- a deliberate substitution from the brief's own "Red-only"
    wording, found and documented rather than silently swapped: gb_paste_hook's
    Gen-1 branch needs a base-stats ROM (`GbGen1Base`, species types/catch rate --
    Gen-1 records carry none of that themselves), and its ONLY lookup path
    (`gb_gen1_locate_rom`, pdna_gen12.c) is an `f_open()` for "<save's own path,
    minus extension>.gb/.gbc" -- an SD-CARD-RELATIVE FatFs open, completely
    independent of the fused-image ROM the box grid's own art reads. PDNA_DELTA has
    no SD card at all, so this lookup ALWAYS fails for a Gen-1 target under this
    harness (G3GB_ERR_NEEDS_BASE -> "NO GEN-1 ROM / Put .gb here..."), before the
    loss screen even renders -- confirmed by actually running this flow against a
    Red-only fused image first and hitting exactly that screen, not an R1 bug.
    Gen 2 needs no base-stats table at all (`gen3_to_gb(..., NULL, ...)` for
    GB_GEN2), so Gold sidesteps the gap entirely and still fully exercises R1's own
    code (the choice/correction logic does not care which Game Boy generation the
    target is).

    Nav: boot picker (row 0 Emerald, row 1 the fused Gen-2 game -- DOWN x1 -> A ->
    box grid (#279: no info page), same shape as run_u4_bag()'s own boot-picker
    sequence) -> R x13 (0-based box index 12) to reach the first box with real
    room on Guy's own Gold.sav -- 17/20, confirmed directly against the save's own
    bytes (tests/host_gbsurgery_tool.c --list). R needs a GENEROUS 200-frame
    settle here, not the usual SETTLE/BIG_SETTLE -- confirmed empirically: shorter
    settles intermittently dropped/misregistered a press with no visible sign
    anything was wrong, landing one or more boxes short of the intended one (an
    early draft of this same probe found a near-empty box mid-search purely by
    accident) -> cursor to slot 17 (one of this box's 3 genuinely-empty real
    slots) -> A -> the empty-cell action menu (CREATE / PASTE HERE / CANCEL)
    -> DOWN x1 -> A -> gen3_to_gb() runs against the clip-seeded Charizard L20 ->
    the EXISTING loss screen (unchanged by R1) -> A (proceed) -> the NEW R1 screen
    (gb_paste_legal_screen): "A = KEEP AS IS" / "SELECT = MAKE LEGAL (20 -> 36)" /
    "B = cancel" -> SELECT -> gb_set_level() raises the level, gb_paste_write()
    commits -> back to the box grid with the corrected Charizard sitting in the
    cell -> VIEW it to show LEVEL 36 landed for real, not just claimed by the dialog.

    R1's own scope note: there is no THIRD screen between the choice and the write
    (the design's #3e "Safety" section: the write is verified/backed-up, not
    re-confirmed a second time) -- "the choice dialog" and "the MAKE LEGAL row with
    a concrete level" are the SAME single screen (05 below); there is no separate
    "confirm" screen to shoot distinctly from it -- selecting SELECT commits
    directly, matching the brief's own "additive, no new screen kind" design. The write itself is proven by the host tests (host_gen3gb_test / host_xfer_roundtrip_test); PDNA_DELTA has no SD., not just in dialog text."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "r1_")
    print("== BACKLOG #104 R1: KEEP AS IS / MAKE LEGAL on a Gen 3 -> Game Boy paste ==")

    boot_to_gb_session(s, rom, which="gold")
    s.shot("01_box_grid", "BACKLOG #104 R1: Gold's box grid, boot-picker -> standalone "
                           "(g_clip pre-seeded with a Charizard at L20 -- fuse_sav.py "
                           "--clip, screenshot-only, no Gen-3 session ever opened). "
                           "BOX1, 20/20 -- no room here, see the R x13 below.")

    # BOX1..BOX12 (0-based box index 0..11) are all 20/20 on Guy's own Gold.sav;
    # BOX13 (0-based index 12) is the first with room -- count=17, slots 17..19
    # genuinely empty within its own 20-slot capacity (confirmed directly: the host
    # surgery tool's --list against the real Gold.sav shows "box 12: count=17,
    # slot 0: dex=241 ... MILTANK", matching what this same R x13 lands on below,
    # species-by-species). R needs a GENEROUS 200-frame settle here, confirmed by a
    # direct probe against known box contents -- shorter settles (BIG_SETTLE=40, or
    # even 80) intermittently dropped presses, landing one or more boxes short with
    # no visible sign anything was wrong (an earlier draft of this same flow found
    # a near-empty box mid-probe purely by accident, not by design).
    s.press_n("R", 13, settle=200)
    # Same drop-prone input path as R above -- DOWN/RIGHT also need a settle well
    # past SETTLE (12 frames): a probe at the default settle landed on slot 13
    # (GYARADOS, still occupied) instead of the intended slot 17, one of THREE
    # RIGHT presses having been swallowed silently. 80 frames, confirmed directly
    # against the known slot contents (slot 16 = LAPRAS, the last occupant; slot
    # 17 = the first empty one), is reliable.
    s.press_n("DOWN", 2, settle=80)
    s.press_n("RIGHT", 5, settle=80)                        # slot 17 -- empty (count=17 here)
    s.shot("02_cursor_on_empty_cell", "BACKLOG #104 R1: BOX13 (R x13 from BOX1, "
                                       "17/20 -- the first box with room), cursor "
                                       "parked on an empty cell before pressing A")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # empty-cell action menu
    s.press_n("DOWN", 1, settle=80)                         # CREATE (default) -> PASTE HERE
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # PASTE HERE -> gen3_to_gb() -> loss screen
    s.shot("03_loss_screen", "BACKLOG #104 R1: the EXISTING loss screen, unchanged -- "
                              "what a Gen3->GB transfer drops regardless of which "
                              "choice R1 adds after it")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # proceed -> the NEW R1 screen
    s.shot("04_legal_choice", "BACKLOG #104 R1: the KEEP AS IS / MAKE LEGAL choice, "
                               "a SEPARATE screen (not a row squeezed onto the loss "
                               "screen, which has no room left -- see the layout "
                               "comment in source/pdna_layout.h). Title 'SEND TO "
                               "GAME BOY'; D3 review row 'CHARIZARD evolves at L36; "
                               "this one is L20.' first (Charizard's checker floor "
                               "IS its true evolution level, so 'evolves at' is the "
                               "honest wording here); row 'A = KEEP AS IS'; row "
                               "'SELECT = MAKE LEGAL (20 -> 36)' -- the concrete "
                               "level this specific underlevelled Charizard needs "
                               "(pk_evo_min_level(6) == 36); D3 review row 'Either "
                               "way it comes back unchanged.' in dim text; 'B = "
                               "cancel' below. This is also the shot for 'the MAKE "
                               "LEGAL row with a concrete level' -- same screen, "
                               "same row.")

    # D3 review: the B-cancel path the review took directly, not just claimed by the
    # dialog's own "B = cancel" hint text -- pressing B here must return to the box
    # grid with NO SIDECAR dialog (gb_paste_hook's own "B here cancels the whole
    # transfer, nothing written" contract, pdna_gen12.c's own comment on this
    # screen), and the cell must still be empty (the sidecar/gbs_insert/gb_persist
    # sequence never ran). Re-entered immediately after so the SAME probe run also
    # covers the MAKE LEGAL path (05/06 below) -- a screenshot session cannot resolve
    # a single dialog instance two different ways.
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # cancel -> back to the box grid
    s.shot("04b_b_cancel", "BACKLOG #104 R1 review: B on the choice screen cancels "
                            "the WHOLE transfer -- back at the box grid, cell 17 "
                            "still the empty-cell action menu (CREATE / PASTE HERE "
                            "/ CANCEL), no SIDECAR dialog and nothing written. "
                            "Re-entering the same cell below to also exercise MAKE "
                            "LEGAL (05/06) -- a single dialog instance cannot be "
                            "resolved both ways.")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # same empty cell -> action menu again
    s.press_n("DOWN", 1, settle=80)                         # CREATE (default) -> PASTE HERE
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # PASTE HERE -> gen3_to_gb() -> loss screen (again)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # proceed -> the R1 screen (again)

    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # choose MAKE LEGAL -> gb_paste_write()
    s.shot("05_sd_refusal_hardware_only", "BACKLOG #104 R1: SELECT registered cleanly "
                           "(no crash, no corruption) and gb_paste_write() ran -- but "
                           "PDNA_DELTA has NO SD card at all, so f_mkdir(PDNA_SIDECAR_DIR) "
                           "always fails here ('SIDECAR FOLDER / Nothing transferred.') -- "
                           "the SAME refusal ANY Gen3->GB write hits in this build, "
                           "pre-existing and unrelated to R1 (the brief's own words: "
                           "'the delta build's in-session refusal keep today's behaviour "
                           "-- the dialog still shows; the refusal follows as before'). "
                           "The actual WRITE landing with the corrected level (36, not "
                           "20) is proven byte-for-byte by the host test instead -- "
                           "tests/host_gen3gb_test.c section 5 and "
                           "tests/host_xfer_roundtrip_test.c section C -- HARDWARE/HOST-"
                           "ONLY proof, flagged per the standing convention, not faked "
                           "here by pretending this emulator wrote to a card it does "
                           "not have.")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -> back to the box grid
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # the SAME cell -> still the empty-cell menu
    s.shot("06_cell_still_empty_no_corruption", "BACKLOG #104 R1: the cell is STILL "
                           "the empty-cell action menu (CREATE / PASTE HERE / CANCEL), "
                           "not a half-written mon -- the refused write left nothing "
                           "behind, matching the sidecar-first safety pattern (hard "
                           "rule 3): a write that cannot land refuses cleanly rather "
                           "than partially committing.")
    return s


def run_r1_xfer_red(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #112: run_r1_xfer() above was shot on GOLD, not Red, because
    gb_gen1_locate_rom's Gen-1 branch was an f_open() over a nonexistent SD card under
    PDNA_DELTA -- every Gen-1 paste target refused with 'NO GEN-1 ROM' before the loss
    screen even rendered (see that function's own docstring for the full story). Now
    that gb_gen1_locate_rom/gb_gen1_base_from_rom fall back to the fused ROM's own
    table under PDNA_DELTA (source/pdna_gen12.c), Red is reachable too -- this is that
    proof: the SAME loss screen + KEEP AS IS / MAKE LEGAL choice run_r1_xfer() shows
    for Gold, now also reached on Red. Trimmed to the reachability proof alone (steps
    01-04 of run_r1_xfer's own sequence) -- the B-cancel round trip, the sidecar
    refusal (pre-existing under PDNA_DELTA, unrelated to #112) and the "cell still
    empty" re-check are run_r1_xfer's own R1 feature coverage, not this fix's.

    `rom` must be `pokedna-delta-artless.gba` fused the same way as run_r1_xfer's own
    image (fuse_sav.py with the SAME --clip charizard_l20.bin), but with
    Red.gb+Red.sav (fuse_gb.py) instead of Gold.

    Nav: boot picker (row 0 Emerald, row 1 the fused Red save) -> R x5 (0-based box
    index 5) to reach BOX6 (17/20 -- box 5, count=16, on Guy's own Red.sav --
    confirmed directly against the save's own bytes, tests/host_gbsurgery_tool.c
    --list: "box 5: count=16", slots 0..15 occupied, slots 16..19 the empty ones) ->
    DOWN x2, RIGHT x4 (same 6-column grid arithmetic run_r1_xfer's own DOWN x2/RIGHT
    x5 uses for slot 17 -- here landing on slot 16, box5's first empty cell) -> A ->
    the empty-cell action menu -> DOWN x1 -> A -> gen3_to_gb() runs against the SAME
    clip-seeded Charizard L20 -> the loss screen -> A -> the R1 screen (KEEP AS IS /
    MAKE LEGAL), reached on Red instead of 'NO GEN-1 ROM'."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "r1_red_")
    print("== BACKLOG #112: Gen-1 (Red) transfer-down now reaches the R1 screen ==")

    boot_to_gb_session(s, rom, which="red")
    s.shot("01_box_grid", "BACKLOG #112: Red's box grid, boot-picker -> standalone "
                           "(g_clip pre-seeded with the SAME Charizard L20 run_r1_xfer "
                           "uses for Gold). BOX1, 20/20 -- no room here, see the R x5 "
                           "below.")

    # BOX1..BOX5 (0-based box index 0..4) are all 20/20 on Guy's own Red.sav; BOX6
    # (0-based index 5) is the first with room -- count=16, slots 16..19 genuinely
    # empty within its own 20-slot capacity. Same generous 200-frame settle as
    # run_r1_xfer's own R x13 -- confirmed there that shorter settles intermittently
    # drop presses with no visible sign anything was wrong.
    s.press_n("R", 5, settle=200)
    s.press_n("DOWN", 2, settle=80)
    s.press_n("RIGHT", 4, settle=80)                        # slot 16 -- empty (count=16 here)
    s.shot("02_cursor_on_empty_cell", "BACKLOG #112: BOX6 (R x5 from BOX1, 16/20 -- "
                                       "the first box with room), cursor parked on an "
                                       "empty cell before pressing A")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # empty-cell action menu
    s.press_n("DOWN", 1, settle=80)                         # CREATE (default) -> PASTE HERE
    # PASTE HERE -> gen3_to_gb() -> G3GB_ERR_NEEDS_BASE -> gb_gen1_base_from_rom() ->
    # THIS fix's PDNA_DELTA fallback -> rom_gbsprite_open() streams the WHOLE 1 MB
    # fused Red ROM once (gb_gen1_locate_rom's own docstring: "romscan ... in ONE
    # pass") -- confirmed by direct probe (host + on-device log_line instrumentation,
    # since removed) to need ~13,000-16,000 frames on real ARM7TDMI-speed emulation,
    # well past BIG_SETTLE (40) -- unsurprising, this is the exact same "cold, slow
    # the first time" ROM scan GB_ART_COLD_SETTLE elsewhere in this file already
    # rides out; only ONE reader (Session.tap) needs it, not a repeat visit (romgs_
    # ready caches the result for the rest of this session).
    s.tap("A", settle=GB_ART_COLD_SETTLE)                   # PASTE HERE -> gen3_to_gb() -> loss screen
    s.shot("03_loss_screen", "BACKLOG #112: the loss screen renders on Red -- NOT "
                              "'NO GEN-1 ROM / Put Red.gb here...', which is what "
                              "this exact flow produced before the fix (the ONLY "
                              "thing #112 changes: the ROM lookup that gates this "
                              "screen, not the screen itself)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # proceed -> the R1 screen
    s.shot("04_legal_choice", "BACKLOG #112: the KEEP AS IS / MAKE LEGAL choice, now "
                               "reached on Red -- proof the fused-ROM fallback in "
                               "gb_gen1_locate_rom/gb_gen1_base_from_rom (source/"
                               "pdna_gen12.c, PDNA_DELTA-only) supplies real base "
                               "stats for a Gen-1 target with no SD card at all")
    return s


def run_s150_10(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-10 (G-H8): PASTE (GB) swaps only the BAD move slots instead
    of refusing the whole record -- gb_paste_legal_screen_ex's swap-row extension of
    the R1 modal above.

    `rom` must be `pokedna-delta-artless.gba` fused exactly as run_r1_xfer()'s own
    docstring prescribes -- GOLD, not Red (same reasoning: a Gen-1 target needs a
    base-stats ROM through a completely different lookup, gb_gen1_locate_rom, that
    this lane deliberately does not touch; Gen 2 needs no base-stats table at all,
    so Gold exercises this lane's own code without that unrelated gap) -- except the
    --clip record is re-moved (tools/extract_gen3_record.c's new --moves flag) to
    {57 SURF, 44 BITE, 317 ROCK TOMB, 182 PROTECT}: on a Gen-2 target only ROCK TOMB
    (317 > gb_max_move(GB_GEN2)==251) is out of range -- Protect (182) is legal in
    Gen 2 -- so this seeds exactly ONE bad slot, the mixed case G-H8 describes.

    Nav: boot picker -> Gold's box grid -> R x13 -> DOWN x2 -> RIGHT x6, to BOX13
    slot 18 (0-based, 6-column grid: row 2 col 6) -> A -> the empty-cell menu ->
    DOWN -> A (PASTE HERE) -> gen3_to_gb_fixed() runs with bad4={0,0,1,0} -> the
    EXISTING loss screen (unchanged) -> A -> the NEW swap-row modal
    (gb_paste_legal_screen_ex).

    BACKLOG #150 S150-10 own re-verification: run_r1_xfer()'s docstring claims slot
    17 (RIGHT x5 from row 2 col 0) is "the first genuinely-empty cell" on Guy's own
    Gold.sav, 17/20 occupied. Re-probed directly against THIS corpus file
    (interactive mGBA session, one RIGHT press at a time, settle=150, a screenshot
    read after every press): row 2 reads ELECTRODE(12)/GYARADOS(13)/EMPTY(14)/
    SNORLAX(15)/SUDOWOODO(16)/LAPRAS(17)/EMPTY(18, labelled "(empty)" on its own
    info panel) -- slot 17 is occupied (LAPRAS), slot 18 is the first genuinely-
    empty cell reachable this way. The corpus file has drifted since run_r1_xfer()
    was written (an extra entry landed somewhere in the box ahead of slot 17,
    without changing the 17/20 header count -- consistent with the box holding a
    scattered empty cell elsewhere in the SAME 20, not only at the tail). RIGHT x5
    at settle=80 (the OLD recipe) also intermittently drops a press on this same
    probe (confirmed directly: two consecutive captures came back pixel-identical
    at that settle) -- both the target slot AND the settle needed a re-verify, not
    just a re-grep. Slot 19 was not probed (18 alone is enough room for one paste)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_10_")
    print("== BACKLOG #150 S150-10: PASTE (GB) swaps only the bad move slot(s) ==")

    boot_to_gb_session(s, rom, which="gold")
    s.shot("01_box_grid", "BACKLOG #150 S150-10: Gold's box grid, boot-picker -> "
                           "standalone (g_clip pre-seeded with a real corpus mon "
                           "re-moved to SURF/BITE/ROCK TOMB/PROTECT -- "
                           "extract_gen3_record --moves, screenshot-only, no Gen-3 "
                           "session ever opened). BOX1, 20/20 -- no room here, see "
                           "the R x13 below.",
           claim=["20/20"])   # BACKLOG #214 item 3 retrofit

    # Re-verified directly against this corpus file (see the docstring above): slot
    # 18 (row 2 col 6), NOT slot 17 -- and settle=150, not 80, or a RIGHT press
    # intermittently drops on this same probe.
    s.press_n("R", 13, settle=200)
    s.press_n("DOWN", 2, settle=150)
    s.press_n("RIGHT", 6, settle=150)                       # slot 18 -- confirmed empty, re-probed directly
    s.shot("02_cursor_on_empty_cell", "BACKLOG #150 S150-10: BOX13 (R x13 from "
                                       "BOX1, 17/20 -- the first box with room), "
                                       "cursor parked on an empty cell (slot 18, "
                                       "re-verified directly against this corpus "
                                       "file) before pressing A")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # empty-cell action menu
    s.shot("03_empty_cell_menu", "BACKLOG #150 S150-10: the empty-cell action menu "
                                  "(CREATE / PASTE HERE / CANCEL), cursor on CREATE "
                                  "(default)",
           claim=["CREATE", "PASTE HERE", "CANCEL"])   # BACKLOG #214 item 3 retrofit

    s.press_n("DOWN", 1, settle=80)                         # CREATE (default) -> PASTE HERE
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # PASTE HERE -> gb_clip_moves+gen3_to_gb_fixed -> loss screen
    s.shot("04_loss_screen", "BACKLOG #150 S150-10: the EXISTING loss screen, "
                              "unchanged by this lane -- gen3_to_gb_fixed() already "
                              "ran with bad4={0,0,1,0} (only ROCK TOMB out of range "
                              "for Gen 2) and emptied that ONE slot rather than "
                              "refusing the whole record with G3GB_ERR_MOVE",
           claim=["WHAT WON'T TRANSFER"])   # BACKLOG #214 item 3 retrofit

    # BACKLOG #150 S150-10 own finding, FIXED by BACKLOG #210: this ONE transition
    # also runs the fill step (decision 8 step 7, gb_paste_fill_moves ->
    # gb_create_locate_rom -> gb_create_learn), which on a COLD session (the ROM's
    # table not yet scanned/cached this session -- gb_create_locate_rom NEVER uses
    # the romgs_ready cache, by design, same as CREATE's own choice) does the SAME
    # "up to ~185,000 read() calls" full-ROM scan CREATE's own s_busy_reading()
    # masks. gb_paste_hook now calls s_busy_reading() immediately before
    # gb_paste_fill_moves() (source/pdna_gen12.c) -- the busy panel draws
    # synchronously (ui_clear + two ui_text calls) BEFORE the scan starts, so a
    # SHORT settle already shows it stably; a much longer settle (the scan itself,
    # measured 20,000+ frames pixel-identical before 4,000) reaches the modal next.
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # proceed -> s_busy_reading() draws, THEN the cold scan starts
    s.shot("04b_busy_reading", "BACKLOG #210: the busy screen (s_busy_reading(), "
                                "CREATE's own panel/text, reused verbatim) now "
                                "covers gb_paste_fill_moves()'s cold "
                                "gb_create_locate_rom() scan -- captured mid-scan, "
                                "BEFORE the swap modal below -- so a cold paste no "
                                "longer looks frozen between the loss screen and "
                                "the modal",
           claim=["Reading your ROM...", "This can take a moment."])

    s.run(4000 - gb_shots.BIG_SETTLE)                       # let the cold scan finish (measured pixel-identical well before 4,000 total)
    s.shot("05_swap_modal", "BACKLOG #150 S150-10 decision 7: the swap-row modal "
                             "(gb_paste_legal_screen_ex) -- ONE row 'ROCK TOMB -> "
                             "<a Gold level-up move>' (the only bad slot; SURF/"
                             "BITE/PROTECT are all <= gb_max_move(GB_GEN2)==251, "
                             "kept as-is); 'KEEP AS IS: not possible here' in dim "
                             "text (nbad > 0 greys this row); 'SELECT = MAKE LEGAL "
                             "(swap moves)' (no level correction alongside this "
                             "paste, so the moves-only wording); 'Either way it "
                             "comes back unchanged.'; 'B = cancel'",
           claim=["ROCK TOMB", "SELECT = MAKE LEGAL", "B = cancel"])   # BACKLOG #214 item 3 retrofit

    # Decision 7: A is not even in the wait mask once any slot is bad -- this tap
    # must be a genuine no-op, asserted by pixel-equality (allow_same=True), not by
    # eye, exactly as the standing rule requires for a claim like this.
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("06_a_is_a_noop", "BACKLOG #150 S150-10 decision 7: A pressed on the "
                              "greyed KEEP AS IS row -- pixel-identical to 05 "
                              "(asserted by the chain itself, not by eye): A is "
                              "simply not in this screen's wait mask once nbad > 0",
           allow_same=True)

    # decision 8 step 7's fills are already in `mon` at this point; SELECT (MAKE
    # LEGAL) only decides whether the record is WRITTEN at all -- gb_paste_write()
    # is UNCHANGED by this lane (decision 8 step 9), and its very first act under
    # PDNA_DELTA is f_mkdir(PDNA_XFER_DIR), which always fails (no SD at all in this
    # build, BACKLOG #62) -- refusing BEFORE gbs_insert() ever runs. Observed
    # directly, matching run_r1_xfer()'s own already-shot precedent exactly (its own
    # shot 05/06 for the SAME reason): "SIDECAR FOLDER / Nothing transferred. /
    # Press A", then back at the box grid with the cell STILL empty. The master
    # design brief's speculative "gb_persist keeps the edit in-session" wording
    # (written for a DIFFERENT refusal point, gb_persist's own card-flush, not
    # gb_paste_write's earlier mkdir gate) does not describe what this build
    # actually shows -- captioned from the real frame, not the brief's guess.
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # MAKE LEGAL -> gb_paste_write()
    s.shot("07_sd_refusal_hardware_only", "BACKLOG #150 S150-10: SELECT registered "
                               "cleanly and gb_paste_write() ran (UNCHANGED by this "
                               "lane) -- but PDNA_DELTA has NO SD at all, so its "
                               "very first act, f_mkdir(PDNA_XFER_DIR), always "
                               "fails here ('SIDECAR FOLDER / Nothing transferred.') "
                               "BEFORE gbs_insert() ever runs -- the SAME refusal "
                               "run_r1_xfer()'s own shot 05 hits, pre-existing and "
                               "unrelated to this lane's move-swap logic. The "
                               "record's fills/pack (decision 8 step 7) are proven "
                               "byte-for-byte by the host tests instead -- "
                               "tests/host_gen3gb_test.c section 7 -- "
                               "HARDWARE-ONLY proof, not faked here.",
           claim=["SIDECAR FOLDER", "Nothing transferred."])   # BACKLOG #214 item 3 retrofit

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -> back to the box grid
    s.shot("08_cell_still_empty_no_corruption", "BACKLOG #150 S150-10: the cell is "
                               "STILL the empty-cell action menu's own EMPTY label "
                               "(not a half-written mon) -- the refused write left "
                               "nothing behind, matching the sidecar-first safety "
                               "pattern (hard rule 3) exactly as run_r1_xfer()'s own "
                               "shot 06 already demonstrated for the level-only case.")
    return s


def run_s150_10_two_bad(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-10: the SAME flow as run_s150_10() above, except `rom`'s
    --clip record is re-moved to {317 ROCK TOMB, 315 OVERHEAT, 0, 0} -- BOTH non-
    empty slots are out of range for Gen 2 (315 > 251 too), so this is the "two bad
    slots, the fills come from Gold's own learnset at the record's level" case --
    the second half of decision 7's worst-case row count. A SEPARATE fused image
    from run_s150_10()'s own (a different --clip payload cannot be swapped mid-
    session), same box/cell coordinates (a fresh Gold.sav copy, same corpus)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_10b_")
    print("== BACKLOG #150 S150-10: PASTE (GB), two bad move slots ==")

    boot_to_gb_session(s, rom, which="gold")
    s.press_n("R", 13, settle=200)
    s.press_n("DOWN", 2, settle=150)
    s.press_n("RIGHT", 6, settle=150)                       # slot 18 -- see run_s150_10()'s own re-verification note
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # empty-cell action menu
    s.press_n("DOWN", 1, settle=80)                         # CREATE (default) -> PASTE HERE
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # PASTE HERE -> loss screen
    s.tap("A", settle=4000)                                 # proceed -> the swap-row modal (cold ROM scan on the first paste this session -- see run_s150_10()'s own note)
    s.shot("05_two_swap_rows", "BACKLOG #150 S150-10 decision 7: TWO swap rows "
                                "('ROCK TOMB -> <fill>', 'OVERHEAT -> <fill>') -- "
                                "both non-empty slots were out of range for Gen 2 "
                                "(317 and 315, both > gb_max_move(GB_GEN2)==251); "
                                "the fills come from Gold's own level-up learnset "
                                "at the record's level (gb_paste_fill_moves, "
                                "decision 5), not a hand-picked pair")
    return s


def run_s150_10_four_bad(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-10 decision 8.7: the SAME flow again, `rom`'s --clip record
    re-moved to four Gen-3-only moves ({317 ROCK TOMB, 332 AERIAL ACE, 339 BULK UP,
    291 DIVE} -- all four > gb_max_move(GB_GEN2)==251) -- the master brief's own
    example for "a mon whose four moves are all > 251". Whether this actually
    reaches decision 8.7's zero-move refusal depends on whether Gold's own
    level-up table has ANYTHING to offer this species at this level -- with a ROM
    present the fill is NOT guaranteed to be empty (a real species almost always
    knows SOME level-up move by its own level), so this is exploratory: the shot
    is taken and captioned from what actually happens, not forced to match the
    refusal. The genuine "no ROM, all four bad" zero-move case is easier to reach
    (an empty learnset always fills nothing) but needs a Gen-2 target with NO Gen-2
    ROM fused at all -- unreachable on THIS harness (booting a Gen-2 session
    requires a Gen-2 ROM to exist in the fused image in the first place) -- see the
    report for why that combination is hardware-only, same posture as the no-ROM
    refusal itself."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_10c_")
    print("== BACKLOG #150 S150-10: PASTE (GB), four bad move slots (exploratory) ==")

    boot_to_gb_session(s, rom, which="gold")
    s.press_n("R", 13, settle=200)
    s.press_n("DOWN", 2, settle=150)
    s.press_n("RIGHT", 6, settle=150)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # empty-cell action menu
    s.press_n("DOWN", 1, settle=80)                         # CREATE (default) -> PASTE HERE
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # PASTE HERE -> loss screen
    s.tap("A", settle=4000)                                 # proceed -> either the modal or the zero-move refusal
    s.shot("05_four_swap_rows_all_filled", "BACKLOG #150 S150-10 decision 8.7: "
                                  "observed result -- FOUR swap rows, ALL filled "
                                  "(ROCK TOMB -> THUNDERSHOCK, AERIAL ACE -> "
                                  "SUPERSONIC, BULK UP -> SONICBOOM, DIVE -> "
                                  "THUNDER WAVE) -- the zero-move refusal does NOT "
                                  "fire here: with a real ROM present, Magnemite's "
                                  "(dex 81, L24) own Gold level-up table has enough moves to fill "
                                  "all four bad slots, exactly decision 8.7's "
                                  "'never block' success path for nbad==4 (every "
                                  "slot bad, but nfill==4 too). The REFUSAL itself "
                                  "(nbad==4 AND nfill==0) needs a no-ROM target, "
                                  "hardware-only on this harness -- see the report.")
    return s


# ---------------------------------------------------------------------------------
# BACKLOG #68b: cold-start timing, WITH vs WITHOUT the fused LOC payloads.
# ---------------------------------------------------------------------------------
# The rest of this file drives NAVIGATION (a fixed settle after each tap, calibrated
# by hand). This section instead MEASURES the thing those settles have to be
# generous enough to survive: the actual frame cost of the box grid's first,
# possibly-cold, art fetch. mGBA's Python binding has no "render finished" signal,
# so "stopped changing for a while" is the proxy -- same posture GB_ART_COLD_SETTLE's
# own 32,000-frame constant was hand-calibrated against.
GBA_FPS = 59.7275006103515625   # GBA's real refresh rate: 2^24 Hz / 280,896 cycles/frame
_MAX_FRAMES = 40000    # generous ceiling over GB_ART_COLD_SETTLE's own 32,000
_SAMPLE_EVERY = 5      # frames between crop samples while polling for "real paint"
_STABLE_WINDOW = 15    # a candidate crop must persist this many frames (3 samples) to count

# The left-panel "currently selected mon" portrait, in screen pixels (240x160) -- the
# ONE region gb_art_fetch()'s cold scan (BACKLOG #62/#68b) actually gates on Red.sav:
# the box grid's own small cell icons come from a separate, always-cheap source and
# are not gated by this backlog item -- but they paint on their OWN schedule, not
# immediately: measured empirically (BACKLOG #68b review D2), the icon cell is blank
# through frame 10, starts painting by frame 15, and is stable by frame 30, on both
# the LOC-seeded and --no-loc builds alike. With the LOC seed's ~10-20-frame portrait
# time, the portrait can appear BEFORE the box-grid icon finishes painting -- the two
# are independent races, not "icon first, portrait second". Measured empirically
# (BACKLOG #68b evidence run, 2026-09): on the SLOWER (--no-loc) build the box grid's
# shell paints almost immediately but this rectangle shows a "loading" checkerboard
# placeholder (colours (231,231,247)/(189,189,206)) for >14,000 frames before the
# real 4-shade Game Boy portrait replaces it; on the faster (default, LOC-seeded)
# build this same rectangle already shows the real portrait within ~20 frames, with
# no checkerboard phase ever observed at this script's sampling granularity.
_PORTRAIT_CROP = (5, 15, 70, 80)


def _crop_bytes(screen, box: tuple[int, int, int, int] = _PORTRAIT_CROP) -> bytes:
    return screen.to_pil().convert("RGB").crop(box).tobytes()


def _derive_checkerboard_ref(core_mod, image_mod, rom: Path) -> bytes:
    """Boots the SLOWER (--no-loc) image and captures _PORTRAIT_CROP's own "loading"
    placeholder, 300 frames after the box grid is requested -- confirmed by a manual
    probe (BACKLOG #68b) to still read as the placeholder that far in, and to stay
    that way for >14,000 further frames on Guy's own Red.gb corpus, i.e. nowhere near
    the real cold-scan actually finishing. Used as one of the two "not real art yet"
    references _measure_box_grid_cold_start() rules out before declaring victory."""
    s = gb_shots.Session(core_mod, image_mod, rom, Path("/tmp"), "measure_ref_")
    s.run(700)
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=300)            # pick Red -> grid (#279: no info page in between)
    return _crop_bytes(s.screen)


def _measure_box_grid_cold_start(core_mod, image_mod, rom: Path,
                                 checkerboard_ref: bytes) -> tuple[int, bytes]:
    """Boots `rom`, drives the boot picker down to Red.sav (row 1), picks it, and
    starts a frame-accurate timer at the exact frame the box grid is REQUESTED (right
    after the key-up edge of the A press, #279: direct entry, no info page). Polls
    _PORTRAIT_CROP every _SAMPLE_EVERY frames until it stops matching `checkerboard_ref`
    (the "loading" placeholder) -- the first frame that is neither the placeholder
    nor a build-specific transition frame. Returns (frames_to_first_paint,
    final_screen_rgb_bytes) -- the second value is for a caller to diff the LOC-seeded
    and scanned paths' full final frames against each other (they must render the
    identical picture, portrait included)."""
    s = gb_shots.Session(core_mod, image_mod, rom, Path("/tmp"), "measure_")
    s.run(700)
    s.tap("DOWN", settle=gb_shots.SETTLE)   # boot picker: Emerald row (0) -> Red row (1)
    # #279: there is no S1 info page any more; the "not real art yet" reference is the boot
    # picker crop (what is on screen when the A press lands).
    info_page_crop = _crop_bytes(s.screen)

    # Same key-down/HOLD/key-up edge tap() uses, but WITHOUT its trailing settle --
    # the timer below starts counting from the exact frame the key-up edge lands,
    # which is the earliest frame the box-grid request could possibly begin.
    s.core.set_keys(raw=gb_shots.KEY["A"])
    s.run(gb_shots.HOLD)
    s.core.set_keys(raw=0)

    # A frame or two of blank/transitional content between "info page" and "real
    # portrait" is normal (the redraw is not atomic) -- a candidate crop value must
    # persist for _STABLE_WINDOW frames once it stops being either "not painted yet"
    # reference before it counts as the real paint, so a one-sample flicker (observed
    # in practice, BACKLOG #68b evidence run) is not mistaken for it.
    frame = 0
    candidate = None
    candidate_since = 0
    while frame < _MAX_FRAMES:
        s.run(1)   # BACKLOG #179 A3 review D2: never advance the core past the VSD
                   # service hook -- a bare core.run_frame() here bypassed
                   # Session.run()'s single funnel, so an in-flight VSD request timed
                   # out and the stale reply then landed in an abandoned buffer.
        frame += 1
        if frame % _SAMPLE_EVERY:
            continue
        cur = _crop_bytes(s.screen)
        if cur == info_page_crop or cur == checkerboard_ref:
            candidate = None
            continue
        if cur == candidate:
            if frame - candidate_since >= _STABLE_WINDOW:
                return candidate_since, s.screen.to_pil().convert("RGB").tobytes()
        else:
            candidate = cur
            candidate_since = frame
    raise RuntimeError(f"{rom}: box grid portrait never left its pre-paint state "
                       f"within {_MAX_FRAMES} frames")


def run_cold_start_compare(core_mod, image_mod, loc_image: Path, noloc_image: Path,
                           out_dir: Path) -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    """BACKLOG #68b: measures and reports the cold-start frame cost of opening Red.sav's
    box grid on `loc_image` (fused WITH the rom_gb*_open_loc() records,
    tools/fuse_gb.py's default) versus `noloc_image` (the same build fused with
    --no-loc, i.e. today's behaviour before this backlog item) -- and proves the two
    images paint the IDENTICAL picture (a byte-diff of the final frame, expected 0)."""
    print("== #68b: cold-start timing, WITH vs WITHOUT the fused LOC payloads ==")
    checkerboard_ref = _derive_checkerboard_ref(core_mod, image_mod, noloc_image)
    loc_frames, loc_px = _measure_box_grid_cold_start(core_mod, image_mod, loc_image, checkerboard_ref)
    print(f"  WITH LOC   : {loc_frames} frames ({loc_frames / GBA_FPS:.2f} s emulated) "
          f"(+/- {_SAMPLE_EVERY} frames sampling granularity)")
    noloc_frames, noloc_px = _measure_box_grid_cold_start(core_mod, image_mod, noloc_image, checkerboard_ref)
    print(f"  WITHOUT LOC: {noloc_frames} frames ({noloc_frames / GBA_FPS:.2f} s emulated) "
          f"(+/- {_SAMPLE_EVERY} frames sampling granularity)")

    diff_bytes = sum(1 for a, b in zip(loc_px, noloc_px) if a != b)
    if len(loc_px) != len(noloc_px):
        diff_bytes += abs(len(loc_px) - len(noloc_px))
    print(f"  full-frame diff  : {diff_bytes} byte(s) differ between the two final frames")

    # The ART this backlog item actually gates on is _PORTRAIT_CROP (the currently
    # selected mon's big portrait, rendered by gb_art_fetch()) -- the rest of the box
    # grid (small cell icons, cursor) animates/updates on its own schedule, unrelated
    # to the cold-scan being measured here, so a whole-frame diff can show a handful
    # of incidental bytes even though the two paths are proven to load through the
    # SAME validated rom_gbsprite_open_loc() call either way. Isolate the claim this
    # backlog item actually makes: is the PORTRAIT itself pixel-identical.
    x0, y0, x1, y1 = _PORTRAIT_CROP
    from PIL import Image
    loc_crop = Image.frombytes("RGB", (240, 160), loc_px).crop(_PORTRAIT_CROP).tobytes()
    noloc_crop = Image.frombytes("RGB", (240, 160), noloc_px).crop(_PORTRAIT_CROP).tobytes()
    portrait_diff = sum(1 for a, b in zip(loc_crop, noloc_crop) if a != b)
    print(f"  portrait-only diff (the {x1 - x0}x{y1 - y0} region gb_art_fetch() actually "
          f"renders): {portrait_diff} byte(s) differ "
          f"({'IDENTICAL' if portrait_diff == 0 else 'MISMATCH'})")

    # Save both final frames as shots, captioned for gb_contact_sheet.py's "#68b:"
    # prefix match (FEATURE_TABLE's own new "delta-gb-loc" row, appended separately).
    ok: list[tuple[str, str, dict]] = []
    from PIL import Image
    for label, px, frames in (("loc", loc_px, loc_frames), ("noloc", noloc_px, noloc_frames)):
        name = f"dgb_coldstart_{label}.png"
        path = out_dir / name
        Image.frombytes("RGB", (240, 160), px).save(path)
        caption = (f"#68b: Red.sav box grid, cold start {'WITH' if label == 'loc' else 'WITHOUT'} "
                   f"the fused rom_gb*_open_loc() record -- {frames} frames "
                   f"({frames / GBA_FPS:.2f} s emulated) to first stable paint")
        ok.append((name, caption, {}))
        print(f"  [ok]   {name:32s} {caption}")
    return ok, []


# BACKLOG #185 D4 (review fix): the Gen-2-specific _derive_checkerboard_ref_crystal()/
# _measure_box_grid_cold_start_crystal() pair that used to live here is GONE --
# folded into _measure_b185_auto() below (parameterised by `down_n`, and fixed to
# auto-detect the F5 fast path instead of assuming every image is slow). Neither
# function was called from anywhere except run_b185_cold_locate(), which now calls
# _measure_b185_auto() directly for both generations.


# BACKLOG #185 D4 (review fix, second pass): the loading placeholder is a KNOWN,
# FIXED 2-colour checkerboard -- (231,231,247) and (189,189,206) -- confirmed by a
# direct probe of a genuinely slow (forced-scan) image's crop at frame 305 (both
# colours, nothing else). Detecting it BY COLOUR, not by sampling a reference crop
# from a separate session, is what actually fixes D4: the first attempt at this fix
# (poll up to a short frame budget, accept whatever stabilizes) failed on the SLOW
# image too, because the placeholder itself stabilizes well within any short probe
# budget -- "stable" alone cannot tell "stable because painted" from "stable because
# it is a persistent loading placeholder" without knowing what the placeholder looks
# like. Colour detection sidesteps the whole fast-vs-slow timing question: a crop is
# "not painted yet" iff it equals info_page_crop OR every pixel in it is one of the
# two checkerboard colours, regardless of how many frames that state lasts -- 10
# frames (F5 fast path) or 14,000+ (a real scan), the same test applies unchanged.
_CHECKERBOARD_COLORS = frozenset({(231, 231, 247), (189, 189, 206)})


def _is_checkerboard(crop: bytes) -> bool:
    for i in range(0, len(crop), 3):
        if (crop[i], crop[i + 1], crop[i + 2]) not in _CHECKERBOARD_COLORS:
            return False
    return True


def _measure_b185_auto(core_mod, image_mod, rom: Path, down_n: int,
                       label: str) -> tuple[int, bytes]:
    """BACKLOG #185 D4 (review fix): measures the box-grid cold-locate cost
    for EITHER the F5 known-ROM fast path (a table hit -- the portrait
    paints within a handful of frames) or a real scan (hundreds to tens of
    thousands of frames, depending on build), without needing to know ahead
    of time which this image is. "Not painted yet" is the known checkerboard
    placeholder (_is_checkerboard() above, by colour, not by a
    separately-sampled reference, #279: no S1 info page transition) -- the first
    frame that is neither the placeholder nor any build-specific transition, held
    stable for _STABLE_WINDOW frames. One session, one pass,
    correct for any speed."""
    s = gb_shots.Session(core_mod, image_mod, rom, Path("/tmp"), f"b185_{label}_")
    s.run(700)
    s.press_n("DOWN", down_n, settle=gb_shots.SETTLE)
    # #279: no S1 info page -- the reference "not painted yet" crop is the boot picker itself.
    info_page_crop = _crop_bytes(s.screen)
    s.core.set_keys(raw=gb_shots.KEY["A"])
    s.run(gb_shots.HOLD)
    s.core.set_keys(raw=0)
    frame = 0
    candidate = None
    candidate_since = 0
    while frame < _MAX_FRAMES:
        s.run(1)   # BACKLOG #179 A3 review D2: never advance the core past the VSD
                   # service hook -- a bare core.run_frame() here bypassed
                   # Session.run()'s single funnel, so an in-flight VSD request timed
                   # out and the stale reply then landed in an abandoned buffer.
        frame += 1
        if frame % _SAMPLE_EVERY:
            continue
        cur = _crop_bytes(s.screen)
        if cur == info_page_crop or _is_checkerboard(cur):
            candidate = None
            continue
        if cur == candidate:
            if frame - candidate_since >= _STABLE_WINDOW:
                return candidate_since, s.screen.to_pil().convert("RGB").tobytes()
        else:
            candidate = cur
            candidate_since = frame
    raise RuntimeError(f"{rom}: {label} box grid portrait never left its pre-paint state "
                       f"within {_MAX_FRAMES} frames")


def run_b185_cold_locate(core_mod, image_mod, noloc_image: Path,
                         out_dir: Path) -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    """BACKLOG #185 Step 1: the emulator floor for locate()'s scan cost alone -- a
    --no-loc fused image has no baked rom_gb*_open_loc() seed, so gb_art_fetch()'s
    cold path (source/gb_art_source.c) runs the REAL scan (F1's gen-restricted 3-job
    scan_multi + F2's inline gates, on the code currently checked out) with ZERO SD
    I/O (PDNA_DELTA reads cart-mapped memory, tools/fuse_gb.py's corpus) -- so
    whatever this measures is CPU cost alone, and the "SD card exonerated" claim in
    the brief can be checked directly: if this floor is already >= ~10 KB/s, the SD
    card was never the bottleneck.

    Measures BOTH generations (Red.sav for Gen 1, Crystal.sav for Gen 2) the same
    way run_cold_start_compare()'s Gen-1-only measurement does, reports frames /
    emulated seconds / bytes-per-second for each, and saves both final frames as
    shots. `noloc_image` must be built the SAME way run_cold_start_compare()'s own
    NOLOC_IMAGE is (`fuse_gb.py --no-loc`), against WHATEVER pokedna-delta.gba the
    caller has just built (i.e. re-run this against a --before and an --after ELF to
    get the comparison the brief's report wants; this function only measures ONE
    image at a time, same as run_cold_start_compare() measures two given images, not
    two builds it builds itself -- building is the caller's job, exactly like that
    function)."""
    print("== BACKLOG #185 Step 1: locate() emulator-floor cold-scan measurement ==")
    from PIL import Image
    ok: list[tuple[str, str, dict]] = []

    frames1, px1 = _measure_b185_auto(core_mod, image_mod, noloc_image, 1, "gen1")
    secs1 = frames1 / GBA_FPS
    bps1 = 0x100000 / secs1     # Red.gb is exactly 1 MiB (rom_gbsprite.c's own header check)
    print(f"  Gen 1 (Red.gb, 1,048,576 B)   : {frames1} frames, {secs1:.2f} s emulated, "
          f"{bps1:.0f} B/s ({bps1/1024:.1f} KB/s) floor")
    name1 = "dgb_b185_gen1_coldscan.png"
    Image.frombytes("RGB", (240, 160), px1).save(out_dir / name1)
    cap1 = (f"#185 Step 1: Red.sav box grid, cold locate() (no .loc seed) -- "
            f"{frames1} frames ({secs1:.2f} s emulated, {bps1/1024:.1f} KB/s floor) "
            f"to first stable portrait paint")
    ok.append((name1, cap1, {}))
    print(f"  [ok]   {name1:32s} {cap1}")

    frames2, px2 = _measure_b185_auto(core_mod, image_mod, noloc_image, 3, "gen2")
    secs2 = frames2 / GBA_FPS
    bps2 = 0x200000 / secs2     # Crystal.gbc is exactly 2 MiB
    print(f"  Gen 2 (Crystal.gbc, 2,097,152 B): {frames2} frames, {secs2:.2f} s emulated, "
          f"{bps2:.0f} B/s ({bps2/1024:.1f} KB/s) floor")
    name2 = "dgb_b185_gen2_coldscan.png"
    Image.frombytes("RGB", (240, 160), px2).save(out_dir / name2)
    cap2 = (f"#185 Step 1: Crystal.sav box grid, cold locate() (no .loc seed) -- "
            f"{frames2} frames ({secs2:.2f} s emulated, {bps2/1024:.1f} KB/s floor) "
            f"to first stable portrait paint")
    ok.append((name2, cap2, {}))
    print(f"  [ok]   {name2:32s} {cap2}")

    return ok, []


def _write_manifest(out_dir: Path, ok: list[tuple[str, str, dict]],
                     skipped: list[tuple[str, str]]) -> None:
    """Same merge-by-file/merge-by-name block tools/gb_shots.py's own main() uses
    (BACKLOG #62 review D3: this script never wrote one at all before). Merged, not
    overwritten, for the identical reason: a partial run must not erase captions a
    separate prior run already wrote.

    BACKLOG #184: `ok` entries are now 3-tuples (file, caption, claim_info) -- claim_info
    is gb_shots.Session.shot()'s own claim_info dict (possibly empty), merged straight
    into the manifest entry: "claim"/"claim_absent" (the original strings, kept so
    --selftest-captions can re-derive pass/fail from the PNG later, offline) and
    "claim_failed" (this capture's own failures, if any -- tools/gb_claims.py). The PNG
    and this manifest entry are written EITHER WAY (a claim failure is still evidence),
    but this function is the ONE place every CLI branch's `_write_manifest(...)` call
    already runs through unconditionally without inspecting a return value -- calling
    sys.exit(1) HERE, after writing, makes a claim failure end the whole process
    non-zero without editing any of the dispatch chain's own `return 0` lines
    (deliberately: other lanes are appending new flags to that chain right now, and a
    touched `return 0` in every existing branch would be a guaranteed merge conflict
    with every one of them)."""
    manifest_path = out_dir / "manifest.json"
    existing = {"shots": [], "skipped": []}
    if manifest_path.is_file():
        existing = json.loads(manifest_path.read_text(encoding="utf-8"))
    by_file = {e["file"]: e for e in existing.get("shots", [])}
    any_claim_failed = False
    for n, c, claim_info in ok:
        entry = {"file": n, "caption": c}
        entry.update(claim_info)
        if claim_info.get("claim_failed"):
            any_claim_failed = True
        by_file[n] = entry
    by_name = {e["name"]: e for e in existing.get("skipped", [])}
    for n, r in skipped:
        by_name[n] = {"name": n, "reason": r}
    manifest_path.write_text(
        json.dumps({"shots": list(by_file.values()), "skipped": list(by_name.values())}, indent=2),
        encoding="utf-8")
    if any_claim_failed:
        print("\n[CLAIM FAILED] one or more shots -- see [CLAIM FAILED] lines above "
              "and each failing entry's manifest.json \"claim_failed\" list", file=sys.stderr)
        sys.exit(1)


def run_s2_bank(core_mod, image_mod, rom: Path, out_dir: Path, which: str,
                 clip_rom: Path = None) -> gb_shots.Session:
    """#120: the Bank, reachable from a Game Boy session (bank_edge's UP
    hop). `rom` MUST be a ONE-ROM fused image matching `which` (tools/fuse_gb.py,
    Gold.gbc+Gold.sav or Red.gb+Red.sav, NO Emerald.sav -- the boot takes the
    fused-GB branch, gb_delta_pick_save() auto-picks the lone entry).

    Reaching the Bank from a fresh box grid entry takes THREE UP presses, not the
    two the brief's own prose named: UP #1 moves the grid cursor (which starts on a
    cell, not the title) up onto the title banner (on_title=true); UP #2 moves from
    the title onto the top tabs (s_tab_focus = 2, SAVE, since a GB source has no
    PARTY tab); UP #3 is the actual bank_edge hop out of tab-focus mode. Verified by
    hand against this exact image (probe screenshots, not guessed) -- the brief's
    count undercounts the tab-focus stage by one press. Each press uses a generous
    100-frame settle: the third one's paint (pdna_bank_show's own f_mkdir/meta_load/
    box_load, all against a card mGBA does not have) needs more than the usual
    BIG_SETTLE=40 to fully resolve, confirmed by hand (40 caught a stale mid-render
    frame in an early probe; 60+ is reliably stable).

    pdna_bank_show() DOES paint in mGBA despite having no SD card: f_mkdir/f_open
    fail silently (FatFs has no disk to find), meta_load() and box_load() both fall
    back to their in-RAM defaults (meta_defaults()/an all-empty box buffer), so the
    Bank opens as an honest "BANK 1  0/30", every cell empty -- not a blank screen,
    not a hang, not an error toast.

    BACKLOG #120 S2 F1 (review finding, closed): CREATE on an empty Bank cell used
    to be unconditional, so a GB session could persist a checksummed Gen-3 record
    built off a zeroed g_vinfo into /PokeDNA/bank/boxNN.box -- a write surface a
    later Gen-3 session's TO GAME could inject into the real save. Now gated
    (xg_create_row): CREATE and PASTE HERE are BOTH absent on an empty Bank cell
    during a GB session, clip-seeded or not, and A is a silent-on-screen, audible
    deny (n == 0, snd_deny() -- app_mon_menu returns false before any popup exists).

    THE KNOCK-ON THIS CLOSES OFF IN mGBA: with CREATE gone, there is no way left to
    get an occupied Bank cell (or anything into your hand) during a GB session's own
    Bank visit in the emulator at all -- pdna_bank_show() unconditionally resets
    g_loaded=-1 on EVERY entry (forcing a fresh box_load() that always fails with no
    card), so even a mon written into the Bank from an ORDINARY Gen-3 session in the
    SAME continuous run does not survive backing out and re-entering the Bank, let
    alone surviving into a LATER, separate GB session. The old d1-d4 CREATE-flow
    shots and the e-series duplicate/cross-gen-deny-with-real-content shots are
    therefore UNREACHABLE in mGBA after this fix -- not a regression, the direct,
    intended consequence of closing the write surface. run_s2_bank_control() below
    proves CREATE + PASTE HERE are UNCHANGED for an ordinary Gen-3 Bank visit; the
    cross-gen deny predicate keeps its own 9/9 unit truth table
    (tests/host_xfergate_test.c); "an occupied Bank cell reached from a GB session"
    and "a Gen-3 DUPLICATE carried into START > GB import" are hardware-only from
    here (docs/HW-QUEUE.md XFER-B10 reworded + the new XFER-B14).

    `clip_rom`, when given, is a SEPARATE run for panel (c): a single-slot,
    no-ROM image (tools/fuse_sav.py --gb ... --clip REC80.BIN) that seeds
    g_clip.occupied so an empty Bank cell's "no CREATE, no PASTE HERE" proves the F1
    gate fired (xg_create_row/xg_paste_row both false) rather than "nothing was ever
    copied" -- fuse_gb.py's own directory format and fuse_sav.py's single-slot
    locator cannot be layered onto one image (pdna_main.c's fused_sav_present()
    check runs BEFORE the gb_delta_pick_save() loop, so a single-slot fusion always
    pre-empts a directory one)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    if clip_rom is not None:
        gb_shots.assert_vehicle(clip_rom, "ARTLESS")  # BACKLOG #255
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"s2bank_{which}_")
    print(f"== #120: the Bank from a Game Boy session ({which}) ==")
    s.run(700)
    # #279: single-ROM image lands on the grid by itself (no S1 info page, no tap)
    s.run(GB_ART_COLD_SETTLE)
    s.shot("a_gb_grid", f"#120: the {which} box grid, freshly entered "
                        "-- every occupied cell wears its own era mark")

    s.press_n("UP", 3, settle=100)                          # grid -> title -> tabs -> the hop (see docstring)
    s.shot("b_bank_hop", "#120: UP x3 from the grid -- the Bank opens "
                         "(\"BANK 1  0/30\", every cell empty -- no SD card in "
                         "mGBA, meta_load()/box_load() both fall back to their "
                         "in-RAM defaults, not a blank screen or a hang)")

    # (c) is a SEPARATE image/session -- see the clip_rom branch below, run first so
    # this function's own `s` (the main a/b/f/g walkthrough) is untouched by it.
    if clip_rom is not None:
        cs = gb_shots.Session(core_mod, image_mod, clip_rom, out_dir, f"s2bank_{which}_clip_")
        cs.run(700)
        # #279: single-slot image lands on the grid by itself (no info page, no tap)
        cs.run(GB_ART_COLD_SETTLE)
        cs.press_n("UP", 3, settle=100)
        cs.tap("A", settle=150)                             # A on the empty cell (cursor starts at 0)
        cs.shot("c_empty_cell_clip_seeded", "#120 F1: A on an EMPTY Bank cell, "
                "with a Gen-3 clipboard entry SEEDED (tools/fuse_sav.py --clip) so the "
                "absence means the F1 gate fired, not \"nothing was ever copied\" -- "
                "the screen is UNCHANGED (still the empty Bank grid, no popup): "
                "xg_create_row/xg_paste_row are both false (no live Gen-3 PC), so "
                "app_mon_menu's n == 0 branch returns before any popup exists, "
                "snd_deny() only (not visible in a screenshot)", allow_same=True)
        s.taken += cs.taken
        s.skipped += cs.skipped

    s.tap("A", settle=150)                                  # A on the (still-empty) main-image cell
    s.shot("c2_empty_cell_no_clip", "#120 F1: the same A-on-empty-cell press on the "
           "MAIN (no-clip) image -- also screen-unchanged, no popup, no CREATE and "
           "no PASTE HERE offered (contrast with c_empty_cell_clip_seeded above: "
           "the result is identical whether or not a clip is seeded, because "
           "CREATE's own gate does not look at the clipboard at all)", allow_same=True)

    s.tap("B", settle=100)                                  # back out of the (still-empty) Bank
    s.shot("f_gb_menu_after_visit", "#120: back on the GB grid after an EMPTY "
           "Bank visit (no occupied-cell menu to show any more -- see the "
           "docstring: F1 closed the only way to get anything into the Bank "
           "during a GB session, so this round trip has nothing to carry). A on "
           "an occupied GB cell still opens the read-only mon menu unaffected "
           "by any of this.")
    s.tap("A", settle=150)
    s.shot("f2_gb_own_menu", "#120: the GB grid's own mon menu, unaffected -- "
           "VIEW/EDIT, LEGALITY, MOVE TO BOX, COPY, RELEASE, CANCEL (+ITEM on Gen 2), same as "
           "before the Bank visit (app_mon_menu_readonly, readonly re-installed by "
           "gb_session_ops_install on the way out)")
    s.tap("B", settle=100)
    s.shot("g_grid_after_visit", "#120: the GB grid after the whole (empty) Bank "
           "visit -- crop-compared against a_gb_grid: every occupied cell's era "
           "mark is pixel-identical (only the cursor position differs); "
           "pdna_bank_show()'s own pdna_origin_box_clear() drops the era cache on "
           "exit, but gb_session_ops_install's hint re-install lets box_decode's "
           "pdna_origin_box_note refill it on re-entry, so nothing visible is lost")
    return s


def run_s2_bank_control(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """#120 F1 control: an ORDINARY Gen-3 (Emerald) session's own Bank visit is
    UNCHANGED by the F1 gate -- xg_create_row(is_bank, pc_live) is `!is_bank ||
    pc_live`, and pc_live (app_gen3_pc_live()) is true throughout a normal session
    (a parsed save, arena free), so CREATE keeps showing on a Bank cell exactly as
    before. `rom` MUST be `tools/fuse_sav.py <pokedna-delta-artless.gba>
    <Emerald.sav>` (a plain Gen-3 fusion, no --gb, no --clip needed -- PASTE HERE is
    proven by COPYing a real party mon in-session instead of a fused clip payload,
    since the ordinary flash-boot/fused-.sav path never reads a fused --clip record
    at all -- only the three PDNA_DELTA GB-boot forks do, source/pdna_main.c's own
    three `fused_clip_present` call sites)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s2bank_control_")
    print("== #120 F1 control: CREATE + PASTE HERE survive an ordinary Gen-3 Bank visit ==")
    s.run(700)
    s.shot("00_boot", "#120 F1 control: Emerald boots straight into the party/box view "
           "(no boot picker -- only one save is fused)")
    s.tap("A", settle=150)                                  # A on the first party mon -> its menu
    s.press_n("DOWN", 4, settle=60)                         # VIEW/EDIT, ITEM, LEGALITY, MOVE -> COPY
    s.tap("A", settle=100)                                  # COPY -> seeds g_clip for real, in-session
    s.shot("01_copied", "#120 F1 control: COPY seeds g_clip the real way (no fused "
           "payload needed for the ordinary flash-boot path)")
    s.tap("A", settle=100)                                  # dismiss the COPIED confirm
    s.tap("B", settle=100)                                  # back to the party/box list
    s.tap("START", settle=80)                               # nav menu
    s.tap("DOWN", settle=60)                                # Party -> Bank (index 1, one DOWN)
    s.tap("A", settle=150)                                  # -> pdna_bank_show(), box 0 (BANK 1)
    s.shot("02_bank", "#120 F1 control: the real Bank, from an ordinary Gen-3 session -- "
           "box 0 shows the PDNA_DELTA plant's 7 native cells (bank_plant_box0's five "
           "directed cells at slots 0-4, plus BACKLOG #150 S150-12 decision 17's two "
           "COPY cells at slots 5/6), 7/30 occupied; cursor on slot 0, a native cell")
    # BACKLOG #198 item 1: slot 0 is a PLANTED native cell (bank_plant_box0(), same
    # navigation run_s2_control() documents) -- landing straight on it and A-ing what
    # the cursor sits on hits the native whitelist menu (VIEW only), not the empty
    # Gen-3 cell's CREATE/PASTE HERE menu this shot means to prove. Land on a REAL
    # empty non-native cell first: DOWN (slot 0 -> slot 6, row 1 col 0 -- ALSO native,
    # decision 17's second COPY cell) then RIGHT (slot 6 -> slot 7, row 1 col 1 --
    # the first genuinely empty, non-native Gen-3 slot), the exact same two-tap
    # recipe run_s2_control()'s own "01_bank_grid" -> "03_empty_cell_menu" chain uses.
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # slot 0 -> slot 6 (row 1, col 0): still native
    s.tap("RIGHT", settle=gb_shots.SETTLE)                  # slot 6 -> slot 7 (row 1, col 1): empty, non-native
    s.tap("A", settle=150)                                  # A on the empty cell -> its menu
    s.shot("03_empty_menu_create_and_paste", "#120 F1 control: slot 7's (the first "
           "genuinely empty, non-native cell) menu -- CREATE, PASTE HERE, CANCEL -- "
           "BOTH present, unchanged by the F1 gate (xg_create_row/xg_paste_row are "
           "true throughout an ordinary Gen-3 session)",
           claim=["CREATE", "PASTE HERE", "CANCEL"])  # BACKLOG #184 retrofit: this is exactly
           # the shot BACKLOG #198 item 1 had to fix (it used to caption a bank_plant.c
           # native cell's VIEW-only menu as this one) -- a mechanical claim is the floor
           # that class of caption lie needs.
    return s


def run_b54_romhack(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #54 T0/T1: the ROM-hack banner + the read-only mon menu app_can_edit()
    and app_src_readonly_set() wire up (review fix F2). `rom` MUST be
    `tools/fuse_sav.py <pokedna-delta-artless.gba> Emerald.sav` (a plain Gen-3
    fusion, no --gb -- same shape as run_s2_bank_control's own vehicle) -- FUSE NO
    GB SAVE: gb_delta_pick_save() (pdna_main.c:8559) takes the GB fork for ANY fused
    GB save, and :8564-8567 is why a single fused GB save skips its own picker,
    which is not the path this lane's banner lives on.

    which="hack": `rom` is a COPY of Emerald.gba (made in /tmp, never in the roms/
    corpus, never committed) with 0xA0..0xAB overwritten "POKEMON HACK" before
    fuse_rom.py ran -- retail (code, version) pair, retail size, WRONG title, so
    rom_open()/rom_identify() classify HACK(EMERALD) (decision 1c). view_save()
    shows the banner right after app_icon_rom_open() runs this session's
    classification.

    A press on a box cell: app_mon_menu()'s pre-existing `if (!app_can_edit())`
    branch (written for cart-wide read-only carts, e.g. Everdrive) still fires
    first, since app_can_edit() is also false for a hack-flagged g_game -- but
    review fix F2 added a check INSIDE that branch: `app_rom_is_hack(g_game) &&
    !g_src_ops` (the `!g_src_ops` guard keeps a MOUNTED GB session, which already
    owns g_src_ops, from being re-routed here, and keeps every OTHER
    !app_can_edit() reason -- Everdrive, pdna_romcheck_bad() -- byte-identical,
    since neither ever calls app_src_readonly_set()) now calls
    app_src_readonly_set(0, PDNA_ROMHACK_NOTE) and enters
    app_mon_menu_readonly() -- VIEW / LEGALITY / COPY / CANCEL, with
    PDNA_ROMHACK_NOTE's prose explaining why. Every row app_mon_menu_readonly()
    can offer here is g_src_ops-gated (ITEM/MOVE/PASTE/CREATE/RELEASE all read
    g_src_ops->*, and this call passes g_src_ops = NULL implicitly), so nothing
    reachable here can mutate g_pc/g_party/g_sb1.

    which="control": `rom` is the UNMODIFIED Emerald.gba -- classifies RETAIL, no
    banner, the ordinary full mon menu (same shape as run_s2_bank_control's own
    "COPY -> ..." menu, proving this lane changes nothing for a genuine retail ROM,
    i.e. decision 1b/retail-pixel-parity)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b54romhack_{which}_")
    print(f"== BACKLOG #54: ROM-hack detection + read-only posture ({which}) ==")
    s.run(700)
    if which == "hack":
        s.shot("a_boot_banner", "BACKLOG #54: view_save()'s msg_wait banner, shown once per "
               "save open when g_game's slot is flagged HACK -- 'ROM HACK' / "
               "'Read-only until verified.' / 'Edits could corrupt this save.' "
               "(PDNA_ROMHACK_TITLE/L1/L2)")
        s.tap("A", settle=150)                                  # dismiss the banner
        s.shot("b_after_dismiss", "BACKLOG #54: A dismisses the banner -- the party/box "
               "view underneath is the ordinary boot screen, EXCEPT the icons are "
               "text name chips (MAG/VOL/...) instead of real ROM sprites -- a real,"
               " correct consequence of this lane: rom_open() refuses the hack "
               "outright (decision 5, it keeps refusing anything but a bit-identical "
               "retail build), so app_icon_rom_open() has no icon source to register "
               "this session, same fallback a genuine no-ROM session already uses")
    else:
        s.shot("a_boot_no_banner", "BACKLOG #54: the unmodified Emerald.gba classifies "
               "RETAIL -- no banner, straight into the ordinary party/box view (retail "
               "pixel parity: this lane must not touch this path at all)")
    s.tap("A", settle=150)                                     # A on the first box cell
    if which == "hack":
        s.shot("c_readonly_menu", "BACKLOG #54 (review fix F2): A on a box cell in a "
               "HACK-flagged save opens app_mon_menu_readonly() -- VIEW, LEGALITY, "
               "COPY, CANCEL only (no EDIT/ITEM/MOVE TO/RELEASE -- g_src_ops is "
               "NULL), with PDNA_ROMHACK_NOTE's prose ('ROM hack: locked') "
               "explaining why editing is off")
    else:
        s.shot("b_mon_menu_normal", "BACKLOG #54: the ordinary full mon menu on the "
               "same save opened against the unmodified ROM -- VIEW/EDIT, ITEM?, "
               "LEGALITY, MOVE TO?, COPY?, RELEASE?, CANCEL, unchanged by this lane")
    s.tap("B", settle=100)                                     # back out, leave no dialog open
    return s


def run_d1_boxname_gate(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """b199-fixes2 D1 fix proof, CORRECTED DESIGN (source/pdna_box.c ~4782): the
    review's first prescription (`else if (src->can_edit && !src->can_edit())
    snd_deny();`) was itself wrong for a Game Boy source -- gbsrc_can_edit() is
    HARDWIRED false unconditionally (nothing to do with writability) and the GB
    source's can_rename is non-NULL, so that branch fired for EVERY GB save and
    swallowed Gen 1's genuine "no table" case (proven live: A on a Red save
    produced no frame change at all under that design). The corrected fix adds a
    field that answers the dialog's actual sentence -- BoxSource.box_names_supported
    (NULL means yes -- Gen-3 PC/Bank always have a table) -- and explains ONLY when
    the table itself is missing:
        else if (src->box_names_supported && !src->box_names_supported())
          { ...explain... }
        else { snd_deny(); }   /* not writable -- stay silent, exactly as main did */

    Three cases, proved here plus a fourth reused from the pre-existing #94 chain:
      (a) which="gen1": Red -- box_names_supported() is non-NULL and false
          (gbbn_supported() refuses, Gen 1 genuinely has no table) -> EXPLAINS.
      (b) which="hack": a hack-flagged Emerald PC -- box_names_supported is NULL
          (pc_box_source leaves it NULL, memset) so the elif never fires; can_rename
          is false (can_edit() false, pdna_romcheck_bad()) so the final `else`
          fires -> SILENT, same as main's box_options_menu path. Silence on a
          screen is ambiguous (correctly-silent vs. the tap being swallowed
          outright), so this case adds a CONTROL TAP (DOWN, which the on_title
          branch handles unconditionally by clearing on_title -- source/pdna_box.c
          ~4734, unrelated to box_names_supported) right after the silent A,
          proving the input loop is alive and the silence is a real "nothing to
          show", not a stuck emulator.
      (c) Gen 2 (Gold) still RENAMES normally -- NOT re-proven here: the
          pre-existing --b94-boxname gold chain (run_b94_boxname, unmodified by
          this lane) already drives this exact path end-to-end (A on the banner ->
          osk_input('BOX NAME', ...) opens -> 'TEST' typed -> banner re-reads
          '1:TEST 20/20') and was re-run against this lane's own build as part of
          this fix's proof; see the D1 report for its frame."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"d1_{which}_")
    print(f"== b199-fixes2 D1: the #244 banner dialog answers the right question ({which}) ==")

    if which == "hack":
        s.run(700)
        s.shot("a_boot_banner", "b199 D1 (hack): view_save()'s ROM-hack banner -- "
               "this save's box source (pc_box_source) has can_edit = app_can_edit, "
               "which is false here (pdna_romcheck_bad() gates on the hack flag); "
               "box_names_supported stays NULL (memset) regardless -- the PC always "
               "has a table, this save's just not writable right now")
        s.tap("A", settle=150)                                  # dismiss the banner
        s.shot("b_box_grid", "b199 D1 (hack): party/box view underneath -- box grid "
               "cur=0, top-left cell")
        s.tap("UP", settle=150)                                 # cur < COLS -> on_title = true
        s.shot("c_title_selected", "b199 D1 (hack): UP selects the TITLE row "
               "(on_title = true) -- A here is the direct rename shortcut this "
               "fix touches")
        s.tap("A", settle=150)                                  # box_names_supported NULL -> final else -> snd_deny()
        s.shot("d_silent_no_dialog", "b199 D1 FIX PROOF (hack): A on the banner -- "
               "box_names_supported is NULL for the PC (always has a table), so "
               "the new explain branch never fires; can_rename() is false (writ"
               "ability), so the final `else { snd_deny(); }` fires -- SILENT, "
               "same as main's own box_options_menu path -- this frame must be "
               "pixel-identical to shot c (no dialog opened)",
               claim_absent=["NO BOX NAMES", "This game has no box names."])
        s.tap("DOWN", settle=150)                                # control tap: on_title branch handles DOWN
                                                                   # unconditionally (pdna_box.c ~4734), proving
                                                                   # the input loop is alive -- distinguishes
                                                                   # "correctly silent" from "tap swallowed"
        s.shot("e_control_tap_moves", "b199 D1 CONTROL (hack): DOWN right after the "
               "silent A -- unconditionally clears on_title and moves the cursor "
               "highlight from the banner onto the grid's top-left cell, a REAL "
               "visible change -- this frame must differ from shot d. Proves the "
               "session's input loop was alive the whole time: shot d's silence "
               "was 'correctly nothing to show', not 'the emulator ate the tap'.")
    else:  # gen1
        boot_to_gb_session(s, rom, which="red")
        s.shot("a_box_grid", "b199 D1 (gen1): Red's box grid, freshly entered -- "
               "cur=0, top-left cell")
        s.tap("UP", settle=100)                                 # cur < COLS -> on_title = true
        s.shot("b_title_selected", "b199 D1 (gen1): UP selects the TITLE row")
        s.tap("A", settle=150)                                  # box_names_supported() false (gbbn_supported)
        s.shot("c_explains_no_dialog", "b199 D1 FIX PROOF (gen1): A on the banner "
               "-- gbsrc_box_names_supported() is g_ed && gbbn_supported(&g_ed->s), "
               "false here (Gen 1 genuinely has no box-name table) and NON-NULL "
               "(the GB source always sets this field), so the new explain branch "
               "fires FIRST, before can_rename is even consulted -- 'NO BOX NAMES "
               "/ This game has no box names.', the only case this message is "
               "honest for. Live proof the corrected predicate is not the review's "
               "first (wrong) prescription: that design's `can_edit()` check would "
               "have caught THIS case too (gbsrc_can_edit() is hardwired false) "
               "and produced the exact same frame for the WRONG reason -- the "
               "distinguishing case is the hack run above, where the two designs "
               "disagree (old: explains and lies; new: silent).",
               claim=["NO BOX NAMES", "This game has no box names."])
        s.tap("A", settle=150)                                  # dismiss
    return s


def run_s150_2_bank_native(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-2: a native ("GBC1") Bank cell renders as the Gen-1/2 Pokemon
    it is. `rom` MUST be `tools/fuse_sav.py <pokedna-delta-artless.gba> <Emerald.sav>`
    (a plain Gen-3 fusion, no --gb, no --clip -- same vehicle as run_s2_bank_control) --
    NO fused payload is needed because the cells themselves come from source/
    bank_plant.c's PDNA_DELTA-only hook in box_load() (source/pdna_bank.c): on this
    build there is never a real box file to read (no SD card at all in a delta image),
    so box 0 plants bank_plant_box0()'s seven directed cells (the five original
    slots 0-4, plus BACKLOG #150 S150-12 decision 17's two COPY cells at slots 5-6)
    and box 1 plants bank_plant_box_full()'s 30-NATIVE worst case (which reuses only
    box0's own five ORIGINAL slots 0-4, then plants 25 fresh non-copy FULL cells at
    5-29 -- box 1 never gets the two COPY cells), automatically, the first time
    either is paged in.

    Same boot/nav recipe as run_s2_bank_control's own docstring: START, DOWN, A ->
    pdna_bank_show(), landing on box 0 (BANK 1 in the on-screen banner) -- ALREADY the
    planted grid, no extra navigation needed to reach it. mGBA timing is NOT the
    measurement the S150-2 acceptance row asks for (a real hardware box-flip timing for
    the 30-native box) -- this panel is a RENDER proof only; the perf numbers come from
    real hardware, reported separately."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_2_")
    print("== BACKLOG #150 S150-2: native Bank cells render + the 30-native worst case ==")
    # CURSOR is a much slower repaint on this screen than an ordinary GB-box cursor
    # move (the left DATA panel redraws through pdna_summary's own species/level/
    # nickname layout, not a quick highlight-only blit) -- SETTLE (12 frames, this
    # file's own "simple cursor move" constant) and even 80 both left the previous
    # frame on screen (found live: a `pixel-identical to the previous shot` abort);
    # 300 is what a debug probe against this exact build confirmed settles fully.
    CURSOR_SETTLE = 300
    s.tap("START", settle=80)                              # nav menu
    s.tap("DOWN", settle=60)                                # Party -> Bank (index 1, one DOWN)
    s.tap("A", settle=150)                                  # -> pdna_bank_show(), box 0 (BANK 1)
    s.shot("00_planted_box0", "S150-2: BANK 1, box_load()'s PDNA_DELTA plant -- seven "
           "native cells at slots 0-6 (CHIKORITA/2, PIKACHU/1, an Egg, an item "
           "holder, the DMG chip at slots 0-4, plus BACKLOG #150 S150-12 decision "
           "17's two COPY cells at slots 5/6), the rest of the grid ordinary empty "
           "Gen-3 slots -- no '?' badge anywhere; every native cell wears its era "
           "mark EXCEPT the DMG cell (species 252 + isBadEgg, D-Q3 -- review F3: no "
           "era to claim)")

    # cursor on each of the five cells in turn -- the left DATA panel (species/level/
    # nickname) is the thing this shot list actually proves: a native cell decodes to
    # a REAL species/level/nickname, not a placeholder or a hole. Slot 0's own panel
    # is ALREADY shown by 00_planted_box0 above (the cursor defaults there on entry),
    # so this only shoots slots 1..4 -- a slot-0 repeat here would be pixel-identical
    # to 00 with no tap in between and trip the same-frame guard (found live).
    labels = ["02_cell1_pikachu", "03_cell2_egg", "04_cell3_item_holder", "05_cell4_dmg"]
    for label in labels:
        s.tap("RIGHT", settle=CURSOR_SETTLE)                # slot i-1 -> slot i
        s.shot(label, f"S150-2: cursor moved one slot right -- the left panel shows "
               "the native cell's own species/level/nickname (05_cell4_dmg: the DMG "
               "chip, nickname DAMAGED)")

    # back to slot 0. A opens the ordinary occupied-mon menu (cursor defaults to row 0,
    # which is the Summary/"VIEW/EDIT" row, Gen 3's own occupied-mon-menu order per the
    # UX-parity audit -- run_gold's own comments above document the same convention);
    # a second A selects it -- app_box_browse's A_SUMMARY case (pdna_main.c) is where
    # this lane's bc_is_native() interception sits, so this opens the REAL Gen-1/2
    # summary (decision 12-13, step 5) instead of pdna_inspect()'s lossy copy.
    s.press_n("LEFT", 4, settle=CURSOR_SETTLE)              # slot 4 -> slot 0
    s.tap("A", settle=150)                                  # slot 0 -> the occupied-mon menu
    s.shot("06_cell0_menu", "S150-2: A on the CHIKORITA cell -- the ordinary occupied-"
           "mon menu, cursor on row 0 (Summary/VIEW-EDIT) -- on/after S150-3 this is "
           "the native WHITELIST (VIEW/EDIT, LEGALITY, MOVE, DUPLICATE, EXPORT, RELEASE, CANCEL -- #271)")
    s.tap("A", settle=150)                                  # select the Summary row
    s.shot("07_cell0_summary", "S150-2: the REAL Gen-1/2 summary, opened read-only over "
           "the native cell's own 80 bytes via gb_native_summary_open() (decision "
           "12-13, step 5) -- NOT pdna_inspect()'s lossy Gen-3-converted copy")
    s.tap("B", settle=150)                                  # out of the summary
    s.shot("08_cell0_back", "S150-2: B returns to the box grid, cursor still on slot 0")

    # cell 4 -- the DMG cell. bc_unpack() itself does not reject on the glitch species
    # (only bc_is_native's magic/ident32/old-build check gates it), so A here is
    # expected to reach the summary over the damaged record, showing whatever garbage
    # decodes out of the 0xFE glitch species -- an honest reflection of "this record
    # decodes but was never a well-formed one", not a refusal.
    s.press_n("RIGHT", 4, settle=CURSOR_SETTLE)             # slot 0 -> slot 4
    s.tap("A", settle=150)                                  # slot 4 -> its occupied-mon menu
    s.shot("09_cell4_menu", "S150-2: A on the DMG cell -- the same occupied-mon menu "
           "shape (review F1: app_mon_menu's occupancy for a native cell now comes "
           "from bc_is_native() -- decoded via pdna_native_cell_decode(), the same "
           "ladder the grid uses -- not from whether pk_decode_mon's meaningless-key "
           "decrypt of the raw bytes happens to pass its checksum) -- on/after S150-3 "
           "this is the native WHITELIST (VIEW/EDIT, LEGALITY, MOVE, DUPLICATE, EXPORT, RELEASE, CANCEL -- #271)")
    s.tap("A", settle=150)                                  # select the Summary row
    s.shot("10_cell4_summary_or_refuse", "S150-2: the Summary row on the DMG cell -- "
           "bc_unpack succeeds (the glitch species lives in list_species/rec, outside "
           "bc_is_native's own magic/ident32/old-build check), so this is expected to "
           "open the summary over the damaged record rather than refuse outright")
    s.tap("B", settle=150)

    # L/R once to the 30-NATIVE worst case (box 1, BANK 2)
    s.tap("R", settle=300)
    s.shot("11_bank2_30native", "S150-2: BANK 2 -- bank_plant_box_full()'s 30-NATIVE "
           "worst case (review F4: 27 FULL + 2 RELAXED + 1 NONE/DMG across the 30 "
           "slots -- it reuses box0's own five directed cells at 0-4, then 25 fresh "
           "FULL cells at 5-29 -- not 30 fresh FULL cells; every render still at "
           "least attempts gen12_can_convert, and 27/30 pay the full PID search, "
           "which is the worst case SS11.9 prices) -- every cell native, every one "
           "wearing its era badge EXCEPT the DMG cell (D-Q3), no '?' anywhere")
    return s


def _count_exact_color(png_path: Path, rgb: tuple) -> int:
    """Exact-match pixel count of `rgb` anywhere in the PNG at `png_path`. Used to pin
    the carry badge's presence quantitatively (BACKLOG #150 S150-13, review D1) --
    counting the grid's own era_cell_mark() fill colour (COL_GEN1/COL_GEN2) is more
    reliable than eyeballing a small OBJ sprite whose fill can read faintly against
    the grass background at a glance."""
    from PIL import Image
    img = Image.open(png_path).convert("RGB")
    return sum(1 for p in img.getdata() if p == rgb)


# COL_GEN1/COL_GEN2 (source/pdna_origin_art.c:52-53) as 8-bit RGB, RGB15 5-bit
# channels scaled by 255/31 and truncated -- the SAME conversion this tool's own
# mGBA/PIL screenshot path produces (confirmed by direct pixel sampling, review D1).
_COL_GEN1_RGB = (90, 173, 74)     # RGB15 11|21<<5|9<<10
_COL_GEN2_RGB = (173, 82, 206)    # RGB15 21|10<<5|25<<10


def run_s150_13_carry_badge(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-13 (#164): the glove shows a carried native Bank cell's era
    badge. Reuses S150-2/S150-3's exact fixture and nav recipe (same box0 plant, same
    --image requirements: a plain tools/fuse_sav.py fusion of an Emerald.sav onto
    pokedna-delta-artless.gba, no --gb, no --clip -- see run_s150_2_bank_native()'s own
    docstring for why no fused payload is needed).

    box0 slot 0 = the Gen-2 CHIKORITA plant (bc_kind() == 2), slot 1 = the Gen-1
    PIKACHU plant (bc_kind() == 1) -- shooting BOTH proves the badge follows bc_kind(),
    not a hardcoded '1'. Before S150-13 this exact sequence's carry frame showed the
    glove with an empty/garbage hand (BACKLOG #164); after, a small badge rides the
    glove's bottom-right corner with the cell's own gen digit in its own gen tint.

    Review D1 (2026-09-22): the settle-the-grid beat in pdna_box.c's idle-bob loop
    could fire the instant a carry began (bob mid-animation at pickup time) and
    re-uploaded the cursor hand pose over the SAME tile ids the badge borrows,
    clobbering it on roughly half of all pickups -- a capture taken immediately after
    MOVE (settle=150, one bob period at most) was parity-blind to this. Frames 01/03
    below are kept for the per-tap trace, but 01b/03b wait >= 60 vblanks (two full bob
    periods) after the pickup, past any settle beat, and PIN the badge's presence by
    an exact pixel count of the grid's own era-tint colour: the grid always contributes
    its own baseline (multiple cells can share a tint), and the badge's own pixels are
    the same exact quantized colour, so badge-present must read STRICTLY higher than
    the pre-carry baseline (00), by the exact number of gen-tint pixels in this file's
    own hand-authored tile (source/box_oam.c's s150_13_badge_tiles)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_13_")
    print("== BACKLOG #150 S150-13 (#164): the glove's carried-native-cell era badge ==")
    s.tap("START", settle=80)                              # nav menu
    s.tap("DOWN", settle=60)                                # Party -> Bank (index 1, one DOWN)
    s.tap("A", settle=150)                                  # -> pdna_bank_show(), box 0 (BANK 1)
    p00 = s.shot("00_box0", "S150-13: box 0, box_load()'s PDNA_DELTA plant -- slot 0 "
           "CHIKORITA (Gen 2), slot 1 PIKACHU (Gen 1); cursor on slot 0",
           claim=["CHIKORITA", "No.152"])
    base_gen1 = _count_exact_color(p00, _COL_GEN1_RGB)
    base_gen2 = _count_exact_color(p00, _COL_GEN2_RGB)
    print(f"  (pixel baseline, no carry) gen1-tint={base_gen1} gen2-tint={base_gen2}")

    # slot 0 (CHIKORITA, gen 2): menu -> MOVE -> carrying.
    s.tap("A", settle=150)                                  # slot 0 -> its whitelist menu
    s.press_n("DOWN", 2, settle=60)                                # VIEW -> LEGALITY -> MOVE
    s.tap("A", settle=150)                                  # select MOVE -> start_carry
    s.shot("01_carrying_gen2", "S150-13: MOVE picked up the Gen-2 CHIKORITA cell, "
           "captured immediately (per-tap trace only -- review D1: this timing is "
           "parity-blind to the settle-beat clobber, see 01b for the pinned proof)")

    # D1: wait past the settle beat (>= 60 vblanks = 2 bob periods) before pinning.
    p01b = s.shot("01b_settled_gen2", "S150-13 review D1: same carry, >= 60 vblanks "
           "later (past any settle-the-grid beat) -- the badge must still be there; "
           "pinned by an exact pixel count below, not eyeballed. Expected to be "
           "pixel-identical to 01 when the cursor itself has not moved (a stable "
           "badge at a stable position is the GOOD outcome, not a stuck frame)",
           settle=60, allow_same=True)
    after_gen2 = _count_exact_color(p01b, _COL_GEN2_RGB)
    delta_gen2 = after_gen2 - base_gen2
    print(f"  (01b) gen2-tint after settle: {after_gen2} (delta {delta_gen2:+d} vs baseline {base_gen2})")
    if delta_gen2 != 22:   # exact index-2 count of s150_13_badge_tiles[1] (the '2'); +7 = a clobbered fragment (review D1 re-verify)
        raise RuntimeError(f"s150_13_01b_settled_gen2: gen2-tint delta {delta_gen2:+d} != expected +22 "
                            f"({base_gen2} -> {after_gen2}) -- the badge is missing or a clobbered "
                            f"fragment (review D1's exact bug)")

    # drop it back on its own slot (self-drop, no write) before moving to slot 1.
    s.tap("A", settle=150)
    s.shot("02_dropped_back", "S150-13: A on the same cell -- drop_held's self-drop "
           "early return, no longer carrying, badge gone")

    # slot 1 (PIKACHU, gen 1): same recipe, one RIGHT first.
    s.tap("RIGHT", settle=300)                              # slot 0 -> slot 1
    s.tap("A", settle=150)                                  # slot 1 -> its whitelist menu
    s.press_n("DOWN", 2, settle=60)                                # VIEW -> LEGALITY -> MOVE
    s.tap("A", settle=150)                                  # select MOVE -> start_carry
    s.shot("03_carrying_gen1", "S150-13: MOVE picked up the Gen-1 PIKACHU cell, "
           "captured immediately (per-tap trace only -- see 03b for the pinned proof; "
           "review D1 found THIS exact frame, on the pre-fix build, showed no badge at "
           "all -- the earlier caption claiming one was FALSE, corrected here)")

    p03b = s.shot("03b_settled_gen1", "S150-13 review D1: same carry, >= 60 vblanks "
           "later -- pinned by an exact pixel count below (expected pixel-identical "
           "to 03 when the badge and cursor are both stable)", settle=60, allow_same=True,
           claim=["PIKACHU"])
    after_gen1 = _count_exact_color(p03b, _COL_GEN1_RGB)
    delta_gen1 = after_gen1 - base_gen1
    print(f"  (03b) gen1-tint after settle: {after_gen1} (delta {delta_gen1:+d} vs baseline {base_gen1})")
    if delta_gen1 != 21:   # exact index-2 count of s150_13_badge_tiles[0] (the '1')
        raise RuntimeError(f"s150_13_03b_settled_gen1: gen1-tint delta {delta_gen1:+d} != expected +21 "
                            f"({base_gen1} -> {after_gen1}) -- the badge is missing or a clobbered "
                            f"fragment (review D1's exact bug)")

    s.tap("A", settle=150)                                  # drop it back on its own slot
    s.shot("04_dropped_back", "S150-13: dropped back, badge gone again")
    return s


def run_s150_3_escape_gate(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-3: the escape-route gate -- the mon-menu whitelist on a native
    Bank cell (VIEW/MOVE/RELEASE/CANCEL only), the deny toast when a native cell in hand
    reaches the drop_held gate (dragged out of the Bank into the PC), and the real
    Gen-1/2 summary opened from the whitelist's own VIEW row. Reuses S150-2's exact
    fixture and nav recipe (same box0 plant, same --image requirements: a plain
    tools/fuse_sav.py fusion of an Emerald.sav onto pokedna-delta-artless.gba, no --gb,
    no --clip) -- see run_s150_2_bank_native()'s own docstring for why no fused payload
    is needed (source/bank_plant.c's PDNA_DELTA-only box_load() hook)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_3_")
    print("== BACKLOG #150 S150-3: the escape-route gate ==")
    s.tap("START", settle=80)                              # nav menu
    s.tap("DOWN", settle=60)                                # Party -> Bank (index 1, one DOWN)
    s.tap("A", settle=150)                                  # -> pdna_bank_show(), box 0 (BANK 1)
    # cursor already on slot 0 (the CHIKORITA plant). A opens the mon menu.
    s.tap("A", settle=150)                                  # slot 0 -> its menu
    s.shot("00_native_menu", "S150-3: A on the CHIKORITA native cell -- decision 7's "
           "WHITELIST, not the ordinary eleven-row occupied menu: VIEW/EDIT / LEGALITY / "
           "MOVE / DUPLICATE / EXPORT .pk / RELEASE / CANCEL (#271), cursor defaults to row 0")

    # VIEW (row 0, already selected) -> A_SUMMARY's dispatch case -> app_box_browse ->
    # its own bc_is_native() interception -> gb_native_summary_open() (D-Q1: the same
    # wrapper S150-2 landed, now reachable through this lane's whitelist instead of a
    # bespoke VIEW action).
    s.tap("A", settle=150)                                  # select VIEW
    s.shot("01_view_native_summary", "S150-3 D-Q1: VIEW opens the REAL Gen-1/2 summary "
           "read-only, via gb_native_summary_open() -- the same wrapper S150-2 landed, "
           "now reached through this lane's whitelist row instead of a bespoke action")
    s.tap("B", settle=150)                                  # out of the summary, back to the grid

    # MOVE (row 1): re-open the menu, DOWN once to MOVE, A to pick the cell up.
    s.tap("A", settle=150)                                  # slot 0 -> its menu again
    s.press_n("DOWN", 2, settle=60)                                # VIEW -> LEGALITY -> MOVE (native menu, #271)
    s.tap("A", settle=150)                                  # select MOVE -> g_move_req -> start_carry
    s.shot("02_carrying", "S150-3: MOVE picked the native cell up into the glove -- "
           "still inside the Bank, s_orig_scope == BOXSCOPE_BANK")

    # DOWN x5: 4 to walk the cursor from slot 0 (row 0) to the bottom row (row 4), a
    # 5th to walk off the Bank's bottom edge -- pdna_box.c's own `else if (src->is_bank)
    # { boxoam_exit(); return 5; }` under KEY_DOWN while still holding, carrying the
    # native cell out into the PC box view.
    s.press_n("DOWN", 5, settle=150)
    s.shot("03_carried_into_pc", "S150-3: carried out of the Bank into the PC box view, "
           "still holding the native cell (src->scope is now BOXSCOPE_PC)")

    # A: attempt to drop onto whatever PC cell the cursor landed on -- drop_held's
    # decision-3 dominating gate (xg_native_escape_denied(s_held, BOXSCOPE_PC)) fires
    # BEFORE any memcpy, msg_wait's PDNA_XFER_NATIVE_TITLE/L1/L2 dialog shows, and the
    # hand is NOT emptied.
    s.tap("A", settle=150)
    s.shot("04_deny_toast", "S150-3 decision 3: dropping a native cell into the PC -- "
           "xg_native_escape_denied() denies BEFORE any 80-byte write, the "
           "'STAYS IN THE BANK' toast shows, and the cell is still in hand (not lost)")
    s.tap("A", settle=150)                                  # dismiss the toast (msg_wait waits for A)
    s.shot("05_still_holding", "S150-3: after the toast, still carrying the same native "
           "cell -- the deny kept the hand full, nothing was written or discarded")

    # Recover to a clean Bank state before the review F1/F2 demos below: UP walks the
    # carry back up through the PC grid -> PARTY tab -> "off PC top -> Bank" (pdna_main.c
    # r==4), then A drops the native cell back on its own origin cell (drop_held's
    # self-drop early return).
    s.press_n("UP", 8, settle=150)
    s.tap("A", settle=150)
    s.shot("06_native_cell_restored", "S150-3: recovery -- the native cell dropped back "
           "on its own slot 0, no longer carried, ready for the review demos below")

    # ---- review F1: the grid's ITEM mode is a fourteenth escape route ----------------
    # Build a real (non-native) mon at the first empty slot -- SLOT 7 (BACKLOG #272:
    # this said "slot 5: box0's plant only populates 0-4", stale since S150-12's two
    # PDNA_DELTA-only COPY cells took slots 5 and 6, so BANK 1 reads 7/30 and RIGHT x5
    # landed on the native COPY cell -- A/A there opened the native cell's VIEW/EDIT and
    # its A on the Name field opened the "Nickname" editor, NOT a CREATE). Slot 7 is
    # row 1, col 1 of the 6-wide grid: DOWN then RIGHT from slot 0. Give it an item via the ordinary ITEM menu row, then TAKE it in
    # the grid's own ITEM cursor mode and try to GIVE/swap it onto the native CHIKORITA
    # cell at slot 0 -- box_set_held()'s bc_is_native() refusal must fire BEFORE any
    # write, with the native cell's own data untouched and the item still in hand.
    s.tap("DOWN", settle=150)                               # slot 0 -> slot 6 (row 1, col 0: COPY cell)
    s.tap("RIGHT", settle=150)                              # slot 6 -> slot 7 (empty)
    s.tap("A", settle=150)                                  # menu on the empty slot
    s.tap("A", settle=200)                                  # CREATE (default selection) -> species picker
    s.tap("A", settle=400)                                  # pick the default species (BULBASAUR) -> summary
    s.tap("B", settle=400)                                  # "Keep this Pokemon?" prompt
    s.tap("A", settle=600)                                  # A = write (backup first)
    s.tap("A", settle=300)                                  # occupied menu on the new mon
    s.tap("DOWN", settle=150)                                # VIEW/EDIT -> ITEM
    s.tap("A", settle=300)                                  # select ITEM -> pick_item
    s.tap("DOWN", settle=150)                                # "?????" (no item) -> MASTER BALL
    s.tap("A", settle=400)                                  # give it MASTER BALL
    s.tap("SEL", settle=100)                                 # cursor mode: NORMAL -> MOVE
    s.tap("SEL", settle=100)                                 # MOVE -> ITEM
    s.tap("A", settle=300)                                  # TAKE the item off slot 7
    s.shot("07_item_taken", "S150-3 review F1: TAKE'd MASTER BALL off the fresh mon in "
           "ITEM cursor mode -- footer reads 'A give  B put back'",
           claim=["A give  B put back"])  # BACKLOG #272 (rule 17): the caption's footer is now mechanically checked
    s.tap("UP", settle=150)                                 # slot 7 -> slot 1 (row 0, col 1)
    s.tap("LEFT", settle=150)                               # slot 1 -> slot 0 (native CHIKORITA)
    s.shot("08_item_cursor_on_native", "S150-3 review F1: still carrying the item, cursor "
           "now on the native CHIKORITA cell")
    s.tap("A", settle=300)                                  # GIVE/swap attempt -> box_set_held() refuses
    s.shot("09_item_give_denied", "S150-3 review F1: A (give/swap) onto the native cell -- "
           "box_set_held()'s bc_is_native() check denies BEFORE any write; pixel-identical "
           "to 08 (a silent snd_deny(), no dialog) -- the native cell and the held item are "
           "both untouched", allow_same=True)
    s.tap("B", settle=300)                                  # put the item back (item_home() skips native slots)
    s.shot("10_item_put_back", "S150-3 review F1: B puts the item back on its real home "
           "slot -- item_home()'s own bc_is_native() skip never considered the native cell "
           "a candidate \"safe home\" in the first place")
    s.tap("SEL", settle=100)                                 # cursor mode: ITEM -> NORMAL (so the next
                                                              # A opens the menu, not another TAKE)

    # ---- review F2: drop_held's same-box SWAP branch gated only the held record -----
    # MOVE the same real mon (now back at slot 7) and try to drop/swap it onto the
    # native CHIKORITA cell in the SAME box -- the destination-side bc_is_native() guard
    # (immediately before the cross-box refusal) must deny before any memcpy, keeping
    # the native cell displayed and the carry still in hand.
    s.tap("A", settle=150)                                  # menu on slot 7 again
    s.press_n("DOWN", 3, settle=150)                        # VIEW/EDIT -> ITEM -> LEGALITY -> MOVE
    s.tap("A", settle=300)                                  # select MOVE -> pick it up
    s.shot("11_carrying_real_mon", "S150-3 review F2: MOVE picked up the real (non-native) "
           "mon from slot 7", claim=["A drop  B cancel"])  # BACKLOG #272
    s.tap("UP", settle=150)                                 # slot 7 -> slot 1
    s.tap("LEFT", settle=150)                               # slot 1 -> slot 0 (native CHIKORITA), same box
    s.shot("12_move_cursor_on_native", "S150-3 review F2: carrying the real mon, cursor now "
           "on the native CHIKORITA cell -- SAME box, so the cross-box refusal below it "
           "does not fire on its own")
    s.tap("A", settle=300)                                  # drop/swap attempt -> the new F2 guard refuses
    s.shot("13_swap_denied", "S150-3 review F2: A (drop/swap) onto the native cell in the "
           "SAME box -- pixel-identical to 12 (a silent snd_deny(), no dialog): "
           "`bc_is_native(recs+cur*80) && !bc_is_native(s_held)` denies before drop_held's "
           "SWAP memcpy runs, so box_save's invariant never has anything to silently refuse",
           allow_same=True)
    s.tap("B", settle=300)                                  # cancel the carry -- origin (box mon) keeps it
    s.shot("14_swap_cancelled", "S150-3 review F2: B cancels the carry -- the real mon "
           "stays at its own slot 7, nothing lost")
    return s


def run_s150_14_native_edit(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-14: editing a native Bank cell in the Game Boy's own summary/
    edit screens. Reuses S150-2/S150-3's exact fixture and nav recipe (same box0 plant,
    same --image requirements: a plain tools/fuse_sav.py fusion of an Emerald.sav onto
    pokedna-delta-artless.gba, no --gb, no --clip) -- see run_s150_2_bank_native()'s own
    docstring for why no fused payload is needed (source/bank_plant.c's PDNA_DELTA-only
    box_load() hook).

    EXPECTATION FOR THE LAST SHOT, stated up front (checked against the shipped source
    before this ladder runs a single tap, not assumed): a delta image has no SD card
    (run_s150_2_bank_native's own docstring says so) and source/pdna_bank.c's box_save()
    has NO PDNA_DELTA branch (`grep -n PDNA_DELTA source/pdna_bank.c` shows only the
    box_load() plant hook at :121-129 and the g_native_snap recompute comment referencing
    it -- nothing in box_save() itself) -- so sf_write_verified() cannot succeed here and
    decision 7's rollback is expected to fire: 07_after_commit proves the ROLLBACK (the
    in-RAM cell restored to its pre-edit bytes), not a real write. Shots 02-06 are what
    prove the edit reached the confirm dialog with the new value."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_14_")
    print("== BACKLOG #150 S150-14: EDITING a native Bank cell in the GB's own screens ==")
    CURSOR_SETTLE = 300
    s.tap("START", settle=80)                              # nav menu
    s.tap("DOWN", settle=60)                                # Party -> Bank (index 1, one DOWN)
    s.tap("A", settle=150)                                  # -> pdna_bank_show(), box 0 (BANK 1)
    # cursor already on slot 0 (the CHIKORITA plant). A opens the mon menu.
    s.tap("A", settle=150)                                  # slot 0 -> its menu
    s.shot("00_menu_view_edit", "S150-14: A on the CHIKORITA native cell -- the native "
           "whitelist's row 0 now reads VIEW / EDIT (Gen 3's own label, decision 5), not "
           "the read-only-only VIEW S150-2/S150-3 shipped")

    s.tap("A", settle=150)                                  # select VIEW / EDIT -> A_SUMMARY -> app_box_browse
    s.shot("01_summary_view", "S150-14: the REAL Gen-1/2 summary opens in VIEW (Gen 3's "
           "own VIEW/EDIT contract: A inside enters edit) -- app_box_browse's native "
           "branch now calls app_native_cell_edit(), which passes allow_edit=true")

    s.tap("A", settle=150)                                  # A inside the summary -> edit mode
    s.shot("02_edit_mode", "S150-14: A entered edit mode -- can_edit was gated inside "
           "gb_native_summary_open (allow_edit && out80 && app_can_edit(), decision 10)")

    s.tap("R", settle=CURSOR_SETTLE)                        # INFO (card 0) -> SKILLS (card 1)
    # card_skills() registers fields in row order: HP's SE0 (slot 0, GBE_DVH is
    # DERIVED and NOT registered -- pdna_gbsummary.c's stat_row(), "derived, shown,
    # not registered"), THEN Atk's DVA (slot 1). fsel starts at 0 (HP's SE row) on
    # entry, so one DOWN is needed to land on a real, editable DV field (Atk/GBE_DVA)
    # before the "a DV change (RIGHT)" step below -- verified live: without this
    # DOWN, RIGHT edits HP's stat-exp, not a DV (found running this exact ladder).
    s.tap("DOWN", settle=CURSOR_SETTLE)                     # HP's SE0 (slot 0) -> Atk's DVA (slot 1)
    s.shot("03_skills_dv", "S150-14: R switched to the SKILLS card in edit mode, DOWN "
           "moved the cursor off HP's stat-exp row (HP's DV is DERIVED, not registered "
           "-- pdna_gbsummary.c's stat_row()) onto Atk's DV row (GBE_DVA), the first "
           "real editable DV field")

    s.tap("RIGHT", settle=CURSOR_SETTLE)                    # gbedit_adjust_checked: +1 on Atk's DV (GBE_DVA)
    s.shot("04_dv_changed", "S150-14: RIGHT on the Atk DV row -- gbedit_adjust_checked "
           "incremented GBE_DVA by one (DV 1 -> 2); `dirty` is now true (memcmp against "
           "the pre-edit shadow). This step ALREADY crosses Chikorita's 87.5% female / "
           "12.5% male ratio -- the header (species/level + gender/shiny, gbe_header, "
           "reads live off `e`'s current DVs every frame) flips F -> M right here")

    # A few more RIGHT presses on the SAME field, past the threshold that already
    # flipped gender at 04 above -- proves the header keeps tracking `e` live rather
    # than freezing at the first change, not a second threshold crossing.
    s.press_n("RIGHT", 10, settle=CURSOR_SETTLE)
    s.shot("05_header_flipped", "S150-14: header still tracks `e` at DV 12 (the gender "
           "flipped at 04 when DV 1 -> 2 crossed the ratio) -- confirms gbe_header reads "
           "live off the edited DVs on every frame, not just the moment they changed")

    # B in EDIT mode (gbsum_edit_keys) only clears `editing` back to VIEW -- it does
    # NOT itself check `dirty` (that check lives in gbsum_view_keys, VIEW mode's own
    # B handler). A second B, now in VIEW mode with `dirty` still true, is what
    # actually calls gbedit_confirm() (verified live: a single B here left the
    # screen in plain VIEW with no popup).
    s.tap("B", settle=150)                                  # edit mode -> view mode (still dirty)
    s.tap("B", settle=150)                                  # view mode, dirty -> gbedit_confirm()
    s.shot("06_confirm_panel", "S150-14: the second B (VIEW mode, `dirty` true) -- "
           "gbsum_view_keys's own KEY_B branch calls gbedit_confirm(c->e), the SAME "
           "write-confirm panel pdna_gbedit() always used (pdna_gbedit.h)")

    s.tap("A", settle=300)                                  # confirm -- *saved = true -> gb_native_summary_open
                                                              # packs via bc_pack, returns true -> app_native_
                                                              # cell_edit's app_xfer_pid_guard -> memcpy -> commit()
    s.shot("07_after_commit", "S150-14: A on the confirm -- back at the box grid. EXPECTED "
           "on a delta image (stated above, before this ladder ran): box_save() has no "
           "PDNA_DELTA branch, so commit() fails and decision 7's rollback restores the "
           "cell's pre-edit bytes -- this shot proves the ROLLBACK path, not a real write "
           "(real-hardware validation, XFER-C23, proves the write)")
    return s


def run_s150_15_view_original(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-15: the GB ORIGINAL row -- a converted Gen-1/2 mon's Gen-3
    menu opens its Game Boy original read-only, with the true origin game and the
    transfer date. `rom` MUST be a PLAIN `tools/fuse_sav.py <pokedna-delta-artless.gba>
    <Emerald.sav>` fusion (no --gb, no --clip -- same vehicle as --s150-2/--s150-8),
    built from a PDNA_DELTA image (decision 13's seam is compiled in). app_met_game()
    on Emerald is 3, the SAME value both the seam hook (pdna_main.c's view_save()) and
    xfer_view.c's own PDNA_DELTA fallback recompute with -- this is WHY the seam's key
    match succeeds (decision 14).

    Nav recipe, verified live against this exact fused image (probe screenshots, not
    guessed):
      Bank entry + native cell VIEW (the parity reference): the SAME three taps
        run_s2_bank_control()'s own "02_bank" uses (START, DOWN, A), then A (slot 0's
        own native whitelist menu) then A (VIEW/EDIT, already selected -- S150-14
        routes this through app_box_browse -> app_native_cell_edit(allow_edit=true),
        so this reference screen's footer reads "A edit  U/D mon  L/R card": can_edit
        is TRUE here, unlike GB ORIGINAL's own structurally-read-only entry point
        (decision 7). The Acceptance section's "pixel-identical except the outline/
        cursor" therefore does NOT hold footer-for-footer on this build (a drift this
        lane's own DRIFT pass did not anticipate, S150-14 having changed what the
        Bank's own VIEW/EDIT row shows since S150-2/3 first shipped it) -- reported,
        not silently matched; the INFO/SKILLS/MOVES card BODIES and the chip/portrait
        are still identical, and the ORIGIN card differs only on the note line, per
        the brief's own claim.
      Back out: TWO B presses (not three -- this build's VIEW/EDIT is not dirty, so
        one B from VIEW mode returns straight to the Bank grid, a second B exits the
        Bank onto the PC box view, box "5.Unp09n" 30/30 on this corpus) -- verified
        live; a third B is pixel-identical to the second (gb_shots.Session's own
        same-frame guard would refuse it).
      Empty PC cell: R x10 -- box 11 ("Qo", 0/30), this corpus's own completely empty
        box (SAME box run_s150_8_gen3_arm's own docstring names for the identical
        reason -- generation-agnostic fixture, same corpus). A on the empty cell (no
        further navigation) offers PASTE HERE -- decision 13's seam at work: the
        PDNA_DELTA hook in view_save() seeded g_clip with a freshly-converted
        CHIKORITA the moment this save loaded.
      Paste: DOWN (CREATE -> PASTE HERE) then A. This triggers a REAL flash write
        (the PC box commits through banksrc_commit -> box_save -> sf_write_verified
        against the emulator's own flash chip, which DOES work on a delta image,
        unlike the Bank's SD-backed native cells) -- "Saving - do not power off" then
        "SAVED / Flash written + verified. No backup in this build." -- ONE more A
        dismisses it before the grid with the pasted mon is visible.
      GB ORIGINAL row position: VIEW / EDIT, ITEM, LEGALITY, GB ORIGINAL, MOVE, COPY,
        PASTE, DUPLICATE, TO DAY-CARE -- DOWN x3 from the menu's own opening selection
        (VIEW / EDIT) lands on GB ORIGINAL, confirmed live.
      Control (an ordinary mon, no row): L x10 from box 11 returns to box 1
        ("5.Unp09n", 30/30) -- the SAME box this whole chain started from, its
        ordinary corpus mons untouched by anything above.
      Bank-abroad row: from the pasted PC mon's own menu, DOWN x5 (VIEW/EDIT, ITEM,
        LEGALITY, GB ORIGINAL, MOVE, COPY) selects COPY; A copies it (a real, in-
        session copy, no fused --clip payload, same posture as run_s2_bank_control's
        own COPY step); back in the Bank, DOWN then RIGHT from slot 0 lands on slot 7
        (this corpus's first empty Bank cell -- verified live, the grid is 6 columns
        wide and slots 0-6 are the seven bank_plant.c cells); A offers PASTE HERE
        (no flash-write dialog this time -- a Bank cell write defers to the box's own
        "BOX NOT SAVED" banner, not an immediate flash commit); DOWN, A pastes; A on
        the result opens its menu, which also lists GB ORIGINAL (decision 1's own
        "a Bank cell that is abroad" scope).

    REVIEW FIXTURE FIX 1 (BACKLOG #150 S150-15 review): a post-S150-9-merge
    ledger-KEY collision (this seam's converted record and S150-9's own planted PC
    box slot 29 both converted bank_plant_cell0()'s IDENTICAL serial-1 bytes, so
    both resolved to the same xr_key_g3(), and xr_open()'s S150-9 shim answered
    first -- shot 06 briefly showed "GOLD (no date)" instead of this seam's own
    date) was fixed by giving this seam its own serial (source/xfer_plant.c's
    XFER_PLANT_SERIAL, 150 -- outside every other PDNA_DELTA fixture's own range),
    via the new bank_plant_cell0_serial(out80, serial) export. The serial lives
    outside every field a summary card draws, so 00-vs-05 parity is unaffected."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_15_")
    print("== BACKLOG #150 S150-15: the GB ORIGINAL row ==")
    SETTLE = 200

    # ---- 00: the Bank's own native-cell VIEW/EDIT (the parity reference) ---------
    s.tap("START", settle=80)
    s.tap("DOWN", settle=60)
    s.tap("A", settle=150)                                   # -> pdna_bank_show(), box 0, slot 0
    s.tap("A", settle=150)                                   # slot 0 -> native whitelist menu
    s.tap("A", settle=150)                                   # VIEW/EDIT -> the REAL Gen-1/2 summary
    s.shot("00_bank_cell0_view", "S150-15: the Bank's own slot-0 CHIKORITA, VIEW/EDIT "
           "-- the PARITY REFERENCE for 05_original_info_card below. Footer reads "
           "'A edit' (can_edit is TRUE here, S150-14) -- GB ORIGINAL's own entry is "
           "structurally read-only instead (decision 7), so that ONE line is the "
           "expected, correct difference, not a bug",
           claim=["CHIKORITA", "A edit"])   # BACKLOG #214 item 3 retrofit
    s.tap("B", settle=150)                                   # VIEW -> Bank grid
    s.tap("B", settle=150)                                   # Bank -> PC box view

    # ---- empty PC cell: PASTE HERE offered by the PDNA_DELTA seam ----------------
    s.press_n("R", 10, settle=150)                           # box 1 -> box 11 ("Qo", 0/30, empty)
    s.tap("A", settle=250)
    s.shot("01_empty_menu_paste_here", "S150-15: A on an empty PC cell -- PASTE HERE "
           "is offered (decision 13's seam: view_save()'s PDNA_DELTA hook seeded "
           "g_clip with a freshly-converted CHIKORITA the moment this save loaded)",
           claim=["CREATE", "PASTE HERE"])  # BACKLOG #221 retrofit: the wrong build
           # vehicle (make delta-gb's combined GB-corpus image instead of this
           # function's own required plain `fuse_sav.py ... --image`, no --gb) lands
           # on a DIFFERENT menu here with no visible symptom until several taps
           # later -- a mechanical claim fails loudly on frame 01 instead.
    s.tap("DOWN", settle=100)                                # CREATE -> PASTE HERE
    s.tap("A", settle=400)                                   # paste -> flash write ("Saving...")
    s.tap("A", settle=250)                                   # dismiss "SAVED / Flash written + verified"
    s.shot("02_pasted_converted", "S150-15: the pasted CHIKORITA now sits in a Gen-3 "
           "PC cell, era badge '2' on its icon (the grid's own GB-origin indicator)")

    # ---- the menu, the row, the read-only summary ---------------------------------
    s.tap("A", settle=250)
    s.shot("03_menu_with_row", "S150-15: A on the pasted mon -- the occupied-cell menu "
           "now lists GB ORIGINAL fourth (VIEW/EDIT, ITEM, LEGALITY, GB ORIGINAL, "
           "MOVE, COPY, PASTE, DUPLICATE, TO DAY-CARE)",
           claim=["GB ORIGINAL"])  # BACKLOG #221 retrofit
    s.press_n("DOWN", 3, settle=80)
    s.shot("04_row_selected", "S150-15: DOWN x3 -- GB ORIGINAL highlighted",
           claim=["GB ORIGINAL"])   # BACKLOG #214 item 3 retrofit
    s.tap("A", settle=300)
    s.shot("05_original_info_card", "S150-15: A -- the REAL Gen-1/2 summary opens over "
           "the ledger's original80, chip reads VIEW, GB2 -- compare to 00 above "
           "(card bodies identical; footer differs by design, see 00's own caption)",
           claim=["VIEW", "GB2"])  # BACKLOG #221 retrofit: PDNA_GBSUM_VIEW_CHIP
           # (pdna_layout.h) + pdna_origin_tag()'s "GB2" (source/pdna_origin_art.c)
    s.press_n("R", 3, settle=SETTLE)                         # INFO -> SKILLS -> MOVES -> ORIGIN
    s.shot("06_original_origin_card", "S150-15: R x3 -- the ORIGIN card, note reads "
           "'GOLD 26-09-16' (decision 13's PLANT_EPOCH date, the origin game the cell "
           "was planted with), 'Sidecar: No' (parity with the native VIEW, open "
           "question 5)",
           claim=["GOLD"])  # BACKLOG #221 retrofit: this exact shot is where the wrong
           # build vehicle (make delta-gb) first produces a same-frame [STOPPED] even
           # at 9e84ec5 (s150-15's own merge) -- see run's own docstring, "no --gb"

    # ---- read-only proof: A and SELECT are both inert -----------------------------
    s.tap("A", settle=250)
    s.shot("07_a_is_inert", "S150-15: A inside -- PIXEL-IDENTICAL to 06 (can_edit is "
           "a literal false; pdna_gbsummary can never set *saved here)", allow_same=True)
    s.tap("SEL", settle=250)
    s.shot("08_select_denied", "S150-15: SELECT -- also PIXEL-IDENTICAL (the flat "
           "editor's own can_edit gate, pdna_gbsummary.c's `if (!c->can_edit) "
           "{ snd_deny(); return false; }`, refuses it; this test cannot hear the "
           "beep, only that no editor opened)", allow_same=True)
    s.tap("B", settle=200)
    s.shot("09_back_on_grid", "S150-15: B -- back on the PC grid (box 11), cursor "
           "still on the pasted mon", allow_same=True)

    # ---- control: an ordinary Gen-3 mon has no row --------------------------------
    s.press_n("L", 10, settle=150)                           # box 11 -> box 1 (ordinary corpus mons)
    s.tap("A", settle=250)
    s.shot("10_control_no_row", "S150-15: A on an ORDINARY box-1 mon (no ledger entry) "
           "-- the same menu family, without GB ORIGINAL (control; not byte-identical "
           "to 03/04 -- this PC (not Bank) mon also carries its own EXPORT .pk row, "
           "TO GAME/EXPORT's own is_bank branch)",
           claim=["MAGNEMITE", "EXPORT .pk"], claim_absent="GB ORIGINAL")   # BACKLOG #214 item 3 retrofit
    s.tap("B", settle=150)

    # ---- a Bank Gen-3 cell that is abroad also shows the row ----------------------
    s.press_n("R", 10, settle=150)                           # box 1 -> box 11 (the pasted mon again)
    s.tap("A", settle=250)                                   # menu on the pasted mon
    s.press_n("DOWN", 5, settle=80)                          # VIEW/EDIT..MOVE -> COPY
    s.tap("A", settle=250)                                   # COPY -> confirm
    s.tap("A", settle=200)                                   # dismiss
    s.tap("B", settle=150)                                   # back to grid
    s.tap("START", settle=80)
    s.tap("DOWN", settle=60)
    s.tap("A", settle=150)                                   # -> Bank, box 0, slot 0
    s.tap("DOWN", settle=100)                                # slot 0 -> slot 6 (row 1, col 0)
    s.tap("RIGHT", settle=100)                                # slot 6 -> slot 7 (this corpus's first empty cell)
    s.tap("A", settle=250)                                   # empty Bank cell menu
    s.tap("DOWN", settle=100)                                # CREATE -> PASTE HERE
    s.tap("A", settle=300)                                   # paste (Bank: no flash-write dialog)
    s.tap("A", settle=250)                                   # A on the Bank-pasted mon -> menu
    s.shot("11_bank_abroad_menu_row", "S150-15: a Bank Gen-3 cell that is abroad "
           "(the pasted CHIKORITA, COPIEd then PASTEd into an empty Bank slot) also "
           "shows GB ORIGINAL -- decision 1's 'a Bank cell that is abroad' scope",
           claim=["GB ORIGINAL"])   # BACKLOG #214 item 3 retrofit
    return s


def run_b190_move_refusal(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #190: Guy's cart report -- "after that promt the screen glitched to show
    both promt and attacks of wigglytuff" -- a move-picker refusal on the GB editor's
    MOVES card left the refusal panel ghosted over the redrawn card underneath it.

    --image MUST be a STANDALONE fuse_gb.py image, ONE payload: a plain `tools/fuse_gb.py
    <pokedna-delta-artless.gba> Yellow.gb Yellow.sav -o out.gba` (Guy's own save/ROM,
    gba-toolkit/roms/gb/ -- copy both to /tmp first, the corpus is read-only). A single
    fused GB payload means gb_delta_pick_save() (pdna_main.c) auto-picks it (n==1), so
    boot lands straight on the box grid (#279: no info page) -- no boot picker to navigate through, same
    convention every other --s150-* flag here documents for ITS own vehicle.

    RECIPE (found live against Yellow.sav's own real box contents, not guessed):
    the box grid opens on whatever box the SAVE's own "current box" field names --
    for this exact Yellow.sav that is the LAST storage box (displayed "12:GB BOX12"),
    empty (0/20). 11 L presses cycle to "1:GB BOX1" (20/20), cursor already on slot 0
    -- Yellow.sav's own #1 BULBASAUR, Lv10, moves TACKLE(33)/GROWL(45)/LEECH SEED(73)/--.
    Mon menu -> VIEW/EDIT (already selected) -> A again enters edit mode (Gen-3 parity,
    same as run_red()'s own 12c step) -> R,R switches INFO -> SKILLS -> MOVES (CARD_MOVES
    == 2, pdna_gbsummary.c) -> fsel 0 is the TACKLE row. A opens pick_move(current=33).

    The DUP refusal (not LATE -- BACKLOG #189 lands first in this lane, so the picker's
    own ceiling already hides every move past gb_max_move(GB_GEN1)==165 and the LATE
    over-range branch in gbedit_press is unreachable from here; move_taken() in
    gb_editor.c excludes the field's OWN slot, so picking TACKLE back into slot 0 is a
    no-op, not a refusal -- GROWL(45), already in slot 1, IS a refusal): sorted by id
    (the picker's default sort), TACKLE(33) and GROWL(45) are 12 ids apart with no
    filtered-out names between them (checked against the real move table, ids 30-50 all
    have real names) -- 12 DOWN presses from the picker's own current-move starting
    selection lands exactly on GROWL, both before and after BACKLOG #189's ceiling
    (filtering only removes ids > 165, never re-orders or removes anything at 33-45).

    THE BUG, confirmed by this exact chain (see the lane's own report for the traced
    root cause): pdna_gbsummary.c's render() only redraws the left mon-portrait rect
    (pdna_summary_draw_left_hint, called when `draw_left` is true) when the CONVERTED
    PkMon actually changed bytes -- `reconv = !shadow_valid || conv_ok != left_ok ||
    memcmp(...)`. A refused edit changes nothing, so `reconv` (and therefore `draw_left`)
    stayed false even though msg_wait()'s own ui_clear() had bumped ui_clear_gen() and
    the outer loop's `full` was correctly true. msg_wait()'s dialog panel (16,48)-(224,
    118) overlaps pdna_summary_bg()'s own excluded left-panel rect (0,11)-(92,150) --
    exactly the #119 trap class (a local partial-repaint shadow not keyed to
    ui_clear_gen()) already fixed three other times in this codebase (pdna_gbflags.c,
    pdna_fly.c, pdna_gbfly.c). THE FIX: `reconv` now ORs in `full` -- whenever a real
    full repaint is due (gen bumped, card changed, editing toggled, first run), the left
    panel redraws unconditionally, the same way pdna_summary_bg()/card body already do.

    Shots: 00 the picker (BACKLOG #189's ceiling header, "MOVES 1-165", visible), 01 the
    cursor moved onto GROWL, 02 the DUP refusal panel, 03 the frame after dismissing it
    -- BEFORE the fix this is where the ghost (ALREADY KNOWN text still on screen,
    MOVES/TACKLE/GROWL/LEECH SEED also on screen) showed up; AFTER the fix it must be a
    clean single screen."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b190_")
    print("== BACKLOG #190: MOVE-PICKER REFUSAL GHOSTING (Yellow.sav, standalone) ==")
    SETTLE = 200
    s.tap("A", settle=SETTLE)                                # info screen -> box grid
    # boots on the save's own "current box" (this Yellow.sav: BOX12, empty) -- 11 L
    # presses cycle to BOX1 (20/20), cursor already on slot 0 (#1 BULBASAUR).
    s.press_n("L", 11, settle=100)
    s.tap("A", settle=SETTLE)                                # slot 0 -> mon menu
    s.tap("A", settle=SETTLE)                                # VIEW/EDIT (already selected) -> summary, VIEW
    s.tap("A", settle=SETTLE)                                # A inside VIEW -> edit mode (Gen-3 parity)
    s.tap("R", settle=SETTLE)                                # INFO -> SKILLS
    s.tap("R", settle=SETTLE)                                # SKILLS -> MOVES; fsel 0 == TACKLE
    s.tap("A", settle=250)                                   # opens pick_move(current=TACKLE/33)
    s.shot("00_picker", "BACKLOG #190 repro: pick_move open on slot 0 (current TACKLE) "
           "-- BACKLOG #189's ceiling header reads \"MOVES 1-165\"",
           claim=["MOVES 1-165"])   # BACKLOG #214 item 3 retrofit
    s.press_n("DOWN", 12, settle=120)                        # id 33 (TACKLE) -> id 45 (GROWL), 12 ids apart
    s.shot("01_on_growl", "cursor moved 12 rows to GROWL (id 45) -- already in this "
           "mon's slot 1, the move that triggers the DUP refusal, not the LATE one "
           "(BACKLOG #189 makes ids > 165 unreachable from this picker)",
           claim=["PP 40"])   # BACKLOG #214 item 3 retrofit: GROWL's own detail-panel PP
           # readout, updated by the cursor move this shot's caption claims -- the
           # highlighted row's own inverse-video "GROWL" text is not a stable claim=
           # target (find() could not match it at any colour on this exact frame,
           # unlike every OTHER row's plain text on the same picker -- left for a
           # future harness-hardening lane, not asserted here without proof)
    s.tap("A", settle=250)                                   # pick GROWL -> gbe_set_move refuses (move_taken)
    s.shot("02_refusal", "ALREADY KNOWN / \"This Pokemon has that move in another "
           "slot.\" -- gbedit_press's DUP branch, msg_wait()'s own panel",
           claim=["ALREADY KNOWN"])   # BACKLOG #214 item 3 retrofit
    s.tap("A", settle=250)                                   # dismiss (msg_wait's wait_keys(KEY_A))
    s.shot("03_after_dismiss", "the frame after dismissing the refusal -- BEFORE the "
           "repaint-contract fix this showed BOTH the refusal panel's leftover text AND "
           "the MOVES card redrawn underneath it (the #190 ghost); AFTER the fix this "
           "is a clean single MOVES-card screen, nothing left over",
           claim=["BULBASAUR"], claim_absent="ALREADY KNOWN")   # BACKLOG #214 item 3 retrofit
    return s


def run_b188_resume_cell(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #188: Guy's cart report -- "tried changing editing wigglytuff... it opens
    on the first pokemkn in the box instead of wigglytuff" -- pdna_box()'s new resume-
    cell hint (app_box_resume_note/_take, pdna_app.h). Same standalone Yellow.gb +
    Yellow.sav image and BOX1 (20/20) navigation as run_b190_move_refusal() -- see that
    function's own docstring for how the 11 L presses land on BOX1 with the cursor on
    slot 0 (#1 BULBASAUR).

    TWO PATHS, both required by the brief:

    (a) SAME-INVOCATION: A on an occupied cell opens the mon menu; B closes it. Both
        happen inside the SAME pdna_box() call (no re-entry through the outer loop) --
        pdna_box.c's own `cur` is simply never reset for this path (BACKLOG #23's own
        shape), so this is a PRE-EXISTING behaviour, pinned here as a control: the
        resume-cell hint must not be needed for it to keep working.

    (b) OUTER RE-ENTRY (the actual #188 fix): START opens the GB nav menu
        (gb_nav_from_start); B backs out of it WITHOUT picking a row. pdna_gen12.c's
        own loop (`for (int r; (r = pdna_box(&s)) != 0; ) { if (r == 2)
        gb_nav_from_start(...); ... s = pdna_gen12_source(m); }`) then calls pdna_box(&s)
        AGAIN -- a genuine new invocation, cur re-declared to 0 at the top of the
        function -- exactly the path whose own comment used to say "there is no
        existing resume this exact cell mechanism" (now updated). Before this fix the
        cursor reset to slot 0 (Bulbasaur) on this return; after it, app_box_resume_take
        (applied because app_box_start_take()'s hint is 0 on every re-entry through this
        loop, and the box matches) resumes slot 3 -- the cell BACKLOG #188 says was lost.

    Shots: 00 cursor on slot 0, 01 three RIGHTs move it to slot 3 (#4 CHARMANDER,
    distinct art from Bulbasaur -- the box art itself proves which cell is selected,
    not just the cursor's own highlight), 02 the mon menu open on slot 3 (path a), 03
    B closes it -- SAME invocation, cursor still slot 3 (the control), 04 START opens
    the nav menu, 05 B backs out of it -- OUTER RE-ENTRY, cursor resumes on slot 3
    (the fix)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b188_")
    print("== BACKLOG #188: pdna_box() RESUME-CELL HINT (Yellow.sav, standalone) ==")
    SETTLE = 200
    s.tap("A", settle=SETTLE)                                # info screen -> box grid
    s.press_n("L", 11, settle=100)                            # BOX12 (empty) -> BOX1 (20/20), cursor slot 0
    s.shot("00_box1_cursor0", "box1, cursor on slot 0 (#1 BULBASAUR)",
           claim=["BULBASAUR"])   # BACKLOG #214 item 3 retrofit
    s.press_n("RIGHT", 3, settle=150)
    s.shot("01_cursor_slot3", "cursor moved to slot 3 (#4 CHARMANDER, distinct art -- "
           "proves the cell, not just the highlight)",
           claim=["CHARMANDER"])   # BACKLOG #214 item 3 retrofit
    s.tap("A", settle=SETTLE)                                 # mon menu on slot 3 (same invocation)
    s.shot("02_menu_open", "mon menu opened on slot 3, same pdna_box() invocation",
           claim=["CHARMANDER"])   # BACKLOG #214 item 3 retrofit
    s.tap("B", settle=250)                                    # close the menu -- SAME invocation
    s.shot("03_same_invocation_back", "(a) B closed the menu, same invocation -- cursor "
           "still slot 3 (CHARMANDER art visible) -- pre-existing behaviour, pinned as "
           "a control, not the fix itself")
    s.tap("START", settle=SETTLE)                             # open the GB nav menu
    s.shot("04_start_menu", "START opened the nav menu -- about to trigger the outer "
           "re-entry loop (pdna_gen12.c's `for (r = pdna_box(&s)) != 0`)")
    s.tap("B", settle=250)                                    # back out with no row picked -> outer re-entry
    s.shot("05_outer_reentry_resumed", "(b) B backed out of the nav menu with no row "
           "picked -- pdna_box() was RE-ENTERED (a genuine new call, cur re-declared "
           "to 0) via the outer loop; BACKLOG #188: the cursor resumes on slot 3 "
           "(CHARMANDER art) instead of resetting to slot 0 (BULBASAUR) the way it did "
           "before app_box_resume_note/_take existed",
           claim=["CHARMANDER"], claim_absent="BULBASAUR")   # BACKLOG #214 item 3 retrofit --
           # the actual #188 fix frame: mechanically proves the resumed cell, not the
           # reset-to-slot-0 regression
    return s


def run_s150_4_uplift(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-4/5 follow-up (lane s150-4-5b), CORRECTED by b199 review D3
    (BACKLOG #199 chain D superseded the grab-time story this docstring used to
    tell): the UP-lift gesture's grab step end to end on `make delta-gb`'s own
    combined image (Emerald.sav + Red/Gold/Crystal) -- boot picker -> Gold's box
    grid -> CM_MOVE grab -> the mon is HELD, no screen at all -- the origin
    prompt / the Bank's price is now paid at the DROP, not the grab (see
    run_s150_9_site2's own docstring for that half, which this chain does not
    reach).

    BACKLOG #171b (this lane, a review finding on top of #171): start_carry()
    (pdna_box.c:1077, at the time) gated the whole lift_up path on the per-
    BoxSource field `src->xfer`, never on the file-static `s_xfer_peer` a Bank
    visit installs. pdna_gen12_source() never assigned it, so every GB grab used
    to fall straight to start_carry()'s plain-memcpy branch -- BoxXferOps.lift_up
    (the origin prompt, the pack, pdna_bank_next_serial()) never ran at all.
    Fixed: `s.xfer = &k_gb_xfer;` in pdna_gen12_source(). THIS WIRING STILL
    MATTERS -- it is what makes the origin prompt and the restore/merge screen
    reachable AT ALL -- but BACKLOG #199's own chain D (0f1e41f, after this
    lane) moved the CALL SITE: start_carry() no longer calls lift_up() for any
    scope, seeded or not (source/pdna_gen12.c's own comment on gb_lift_pack:
    "the call moved from grab time... to the one drop that actually needs the
    Bank's price"). So the grab itself never shows the picker or a refusal any
    more -- it is now indistinguishable from an ordinary Gen-3 grab, footer
    "A drop  B cancel", nothing else. #171b's fix is still live and still
    load-bearing; it just fires one drop later than this docstring used to say.

    THE GRAB NOW JUST HOLDS -- footer "A drop  B cancel", no full-screen picker,
    no box sprites disturbed. Re-derived live against this exact vehicle (not
    assumed): A on slot 0 (CM_MOVE) produces this frame and nothing else.

    A SECOND A ON THE SAME CELL IS A NO-OP, NOT A REFUSAL -- drop_held's own
    guard (source/pdna_box.c ~1586: `if (s_orig_slot >= 0 && same_scope(src) &&
    s_orig_box == box && cur == s_orig_slot) { s_holding = false; *done = true;
    return recs; }`) fires before ANY write is attempted -- "dropped back on its
    own cell". No meta_save(), no serial, no snd_deny(), no repaint trick: the
    screen returns to plain CM_MOVE, empty-handed ("MOVE  A grab  hold=set"),
    because there was never anything to refuse. This superseded a REAL prior
    fact (BACKLOG #171b's original find: picking GOLD on the origin picker used
    to reach pdna_bank_next_serial(), whose meta_save() fails with no SD card on
    this delta vehicle, and the grab was refused there, silently) -- but that
    call no longer happens from this cell at all; it is a different code path
    now (run_s150_9_site2's Bank-drop chain still exercises the real refusal).

    WHAT THIS CHAIN NO LONGER PROVES, HONESTLY NAMED: neither "the origin prompt
    draws" nor "the grab is silently refused" is a fact about THIS gesture (grid
    A on slot 0) any more -- both moved to the Bank-drop gesture
    run_s150_9_site2 covers. What this chain DOES still prove: the grab is a
    real hold (proof below plus BACKLOG #173 F3's tab-focus-while-empty-handed
    facts, unaffected by chain D since this vehicle's self-drop path never lets
    the hold survive to be inspected while carrying).

    NOT in this chain: the Gen-1 last-party-mon (party-floor) refusal. Guy's own
    Red.sav (the corpus `make delta-gb` fuses) carries a FULL 6/6 party -- lifting any
    one of six never crosses the party floor, so the refusal is not reachable with
    this exact corpus without save surgery this lane does not perform. Left for real
    hardware (or a purpose-built 1-mon-party fixture), not faked here."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_4_")
    print("== BACKLOG #150 S150-4/5 follow-up: the grab step (b199 review D3: grab "
          "now just holds, no picker) ==")
    boot_to_gb_session(s, rom, which="gold")
    s.shot("00_gold_box_grid", "s150-4: Gold's box grid, freshly entered -- cursor on "
           "slot 0 (No.1 BULBASAUR), footer 'A menu  SEL  L/R  B'")

    s.tap("SEL", settle=100)
    s.shot("01_cm_move", "s150-4: SELECT cycled the cursor mode to MOVE (a GB grid "
           "cycles NORMAL<->MOVE only, decision 8(b)) -- footer 'MOVE  A grab  hold=set'")

    s.tap("A", settle=150)
    s.shot("02_grabbed_no_screen", "s150-4 (b199 review D3 repair): A grabbed slot "
           "0 -- footer 'A drop  B cancel', NO screen at all. BACKLOG #199 chain D "
           "moved BoxXferOps.lift_up (the origin prompt, the pack, "
           "pdna_bank_next_serial()) off the grab entirely -- start_carry() is now "
           "a plain memcpy for every scope, exactly the shape it had BEFORE #171b, "
           "except the difference now lives at the DROP (run_s150_9_site2), not "
           "here. This frame used to show the full-screen 'WHICH GAME IS THIS?' "
           "picker; it no longer does, and that is correct, not a regression.",
           claim=["A drop  B cancel"])

    s.tap("A", settle=200)
    s.shot("03_self_drop_noop", "s150-4 (b199 review D3 repair): A on the SAME "
           "cell again -- drop_held's own same-cell guard (source/pdna_box.c "
           "~1586: s_orig_box==box && cur==s_orig_slot) fires BEFORE any write is "
           "attempted -- 'dropped back on its own cell', s_holding=false, *done= "
           "true. No meta_save(), no serial, no snd_deny(): there was never "
           "anything to refuse. Back to plain CM_MOVE, empty-handed ('MOVE  A "
           "grab  hold=set'), No.1 BULBASAUR still at slot 0. This superseded a "
           "REAL prior fact (BACKLOG #171b's own find: picking GOLD on the origin "
           "picker used to reach pdna_bank_next_serial(), whose meta_save() fails "
           "with no SD card here, refusing the grab) -- that refusal is real but "
           "no longer reachable from THIS gesture; run_s150_9_site2's Bank-drop "
           "chain is where it lives now.",
           claim=["MOVE A grab hold=set"])

    # BACKLOG #173 F3 (review-sonnet A5, 2026-09-21), RE-DERIVED for b199 review D3
    # (the ORIGINAL reasoning below is stale -- kept struck through in spirit, not
    # in fact, so the next reader does not have to re-derive the correction): the
    # brief asks this chain to lift a GB mon then press UP to reach tab focus WHILE
    # HOLDING, to capture the carry-aware footer (PDNA_TAB_FOCUS_CARRY_FOOTER,
    # "L/R tab  UP bank  DN"). That literal sequence is attempted below, and it
    # STILL cannot be produced by this chain -- but the REASON changed. The old
    # claim was "the grab is REFUSED before s_holding is ever set true: every
    # BOXSCOPE_GB grab routes through src->xfer->lift_up, which needs a real SD
    # write" -- FALSE as of BACKLOG #199 chain D: start_carry() no longer calls
    # lift_up() at grab time for ANY scope, so shot 02 above holds for real
    # (s_holding IS true there). What actually clears s_holding before this UP is
    # shot 03's SELF-DROP NO-OP (drop_held's same-cell guard, source/pdna_box.c
    # ~1586) -- a second A on the SAME cell was always going to land back on
    # itself with this chain's own tap sequence, chain D or not; this was never a
    # refusal, just this chain never dropping the mon ANYWHERE else. A genuine
    # held-while-UP frame still needs the mon to survive to a DIFFERENT cell or
    # tab press before UP, which this chain's own tap sequence does not attempt
    # (run_s150_9_site2's chain proves a real hold survives a 2xUP Bank hop
    # instead) -- so the carry-aware footer stays HARDWARE-ONLY *for this
    # specific chain*, not because mGBA cannot emulate the write (CLAUDE.md rule
    # 17 no longer the operative reason here), but because this chain's own tap
    # sequence never keeps the mon held past this point. What follows presses UP
    # anyway and shows exactly what happens on THIS vehicle: still empty-handed
    # (shot 03 already cleared s_holding), so KEY_UP takes the ORDINARY
    # (non-holding) grid-navigation path (pdna_box.c:4067 `if (cur < COLS)
    # on_title = true`, then :4026 `s_tab_focus = src->is_bank ? 2 : 1`) rather
    # than the holding-only tab-focus edge (pdna_box.c :3906-3909) the brief had
    # in mind. Both UP presses use settle=100, matching run_b142_tab_focus_
    # arrival's own established idiom just above this function (its own comment:
    # entering tab focus triggers a full repaint, which the default SETTLE=12
    # "simple cursor move" budget is too short to catch cleanly -- confirmed live:
    # a first pass at SETTLE=12 captured three consecutive frames with an
    # unchanged footer despite the title bar's icons visibly changing underneath,
    # i.e. the repaint's regions land on different frames; settle=100 closes that
    # gap, exactly as the existing b142/s150-7/s150-8 chains already do for every
    # tab-focus-entering UP in this file).
    s.tap("UP", settle=100)
    s.shot("04_up_after_refusal_title_row", "s150-4/BACKLOG #173 F3: still empty-handed "
           "after the self-drop no-op (shot 03) -- UP from grid row 0 takes the ORDINARY "
           "non-holding path (on_title = true), NOT the holding-only tab-focus edge; "
           "footer 'L/R  A name  SEL  menu' (on_title's own line, not tab_focus_footer's) "
           "-- cursor now on the box title row")

    s.tap("UP", settle=100)
    s.shot("05_tabfocus_emptyhanded", "s150-4/BACKLOG #173 F3: a second UP from the "
           "title row reaches tab focus (s_tab_focus = src->is_bank ? 2 : 1; a GB "
           "source is is_bank=true, so this lands on tab 2/SAVE) -- empty-handed, so "
           "draw_footer() calls tab_focus_footer(s_holding=false, ...), which always "
           "returns the plain 'L/R tab  A pick  DN' regardless of carry_is_gb (see "
           "source/tab_focus_footer.c) -- footer confirmed live as exactly that text. "
           "This is the reachable half of F1's fix: the holding=false branch, unchanged "
           "by the A1 fix, rendering correctly in situ. The holding=true branch (the "
           "line F1 actually changed, PDNA_TAB_FOCUS_CARRY_FOOTER) needs a real held "
           "GB-origin carry to exercise on screen -- HARDWARE-ONLY, see above; "
           "host_tabfocusfooter_test.c pins both branches directly instead (0 failed, "
           "plus the mutation run reverting the fix fails 1/4 checks on the real "
           "source -- see this lane's report).")

    s.tap("B", settle=100)
    s.shot("06_footer_restored", "s150-4/BACKLOG #173 F3: B backs out of tab focus -- "
           "pdna_box.c:3962 `s_tab_focus = -1` returns to the box TITLE row (on_title "
           "stays true, this branch never touches it), so the footer is on_title's own "
           "'L/R  A name  SEL  menu' again, matching shot 04 -- s_tab_focus is back to "
           "-1, confirming the footer-restore edge itself is intact and live")
    return s


def run_b166(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #166: a Gen-1/2 grid cell whose lift would be refused must not offer
    MOVE TO BOX at all, and one grey line in the panel must name why
    (AppSrcOps.lift_why, source/pdna_app.h; gb_lift_why_hook/gb_lift_why_bs,
    source/pdna_gen12.c; app_mon_menu_readonly's RO_MOVE row, source/pdna_main.c).

    THE REFUSED-LIFT CASE COULD NOT BE PRODUCED WITH THIS CORPUS -- checked, not
    guessed: a standalone C harness (/tmp/probe_lift.c, this lane, not shipped)
    linked straight against source/gb_session.c + source/gen1_save.c +
    source/gen2_save.c + source/gen1_write.c + source/gen2_write.c + source/gb_edit.c
    opened Guy's own Red.sav/Gold.sav/Crystal.sav (the exact saves `make delta-gb`
    fuses) and called gbs_box_writable()/gbs_can_delete() -- the SAME two functions
    gb_lift_why_bs calls -- for every box (0..nboxes) and the party pseudo-box, every
    occupied slot in each: ZERO refusals anywhere in any of the three saves. This
    matches run_s150_4_uplift()'s own documented finding for Red.sav alone ("Guy's
    own Red.sav carries a FULL 6/6 party -- lifting any one of six never crosses the
    party floor") and extends it: Gold.sav/Crystal.sav have no Mail-holding party
    member and no unwritable box either, in this corpus. A GBS_ERR_PARTY_FLOOR /
    GBS_ERR_MAIL / GBS_ERR_UNWRITABLE demonstration needs a purpose-built 1-mon-party
    (or Mail-holding, or virgin-Gen-1-bank) fixture, or real hardware with such a
    save -- left for hardware/a future fixture, NOT faked here (the same posture
    run_s150_4_uplift's own party-floor note already set for this corpus).

    What IS shown, live: a NORMAL (liftable) cell's occupied-cell menu is byte-for-
    byte the shape it was before this lane -- MOVE TO BOX still offered, no grey
    line -- proving the new lift_why gate is a pure ADDITION on the refusal path,
    not a regression on the everyday one."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b166_")
    print("== BACKLOG #166: RO_MOVE's lift_why gate -- normal cell unchanged; "
          "refused-lift case not reproducible with this corpus (see docstring) ==")
    boot_to_gb_session(s, rom, which="red")
    s.shot("00_box_grid", "b166: Red's box grid, freshly entered -- cursor on slot 0",
           claim=["SLOWBRO"])   # BACKLOG #214 item 3 retrofit

    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("01_normal_cell_menu", "b166: A on slot 0 (a normal, liftable cell) -- "
           "the occupied-cell menu is UNCHANGED by this lane: VIEW/EDIT, LEGALITY, "
           "MOVE TO BOX, COPY, RELEASE, CANCEL, no grey reason line -- lift_why "
           "returned NULL (gbs_can_delete == GBS_OK for this slot, verified above), "
           "so the row-omission path in app_mon_menu_readonly never triggers here",
           claim=["MOVE TO BOX", "RELEASE"])   # BACKLOG #214 item 3 retrofit -- both rows
           # this lane's gate could have hidden are mechanically proven still present
           # on a normal (unrefused) cell
    s.tap("B", settle=100)
    s.shot("02_back_to_grid", "b166: B backs out of the menu, box grid unchanged")
    return s


def run_b182_release_gate(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #182 (b219, RELEASE mirrors #166's RO_MOVE gate): a Gen-1/2 party cell
    whose delete would be refused (PARTY_FLOOR -- a 1-mon party, gbs_can_delete's own
    "the party needs one Pokemon" rule) must not offer RELEASE either -- it used to
    open the flow (gb_release_hook -> gb_release_confirm -> gbs_delete) and only get
    refused at the very end, the exact same late-bounce shape #166 fixed for MOVE.
    The fix (source/pdna_main.c, app_mon_menu_readonly's RO_RELEASE row) reuses
    AppSrcOps.lift_why (no separate release_why hook needed: gb_release_hook ends in
    the identical gbs_can_delete() table gb_lift_why_bs already wraps for lift_why) --
    a `release_why` local gates the row exactly like MOVE's own `move_why`.

    run_b166()'s own docstring documented that Guy's real corpus saves (Red/Gold/
    Crystal, all full 6-mon parties) can never reach the PARTY_FLOOR branch, and left
    a purpose-built 1-mon-party fixture for later -- this is that fixture:
    tests/host_gbsurgery_tool.c trimmed a /tmp copy of Red.sav's party from 6 to 1
    (`--op delete party 5/4/3/2/1`, deleting from the END so earlier slot indices
    stay valid; MEW, the party's own slot 0, is the one mon left) via `--out`, then
    tools/fuse_gb.py fused it standalone onto pokedna-delta-artless.gba with Red.gb
    (ONE payload, same shape run_b190_move_refusal() uses -- gb_delta_pick_save()
    auto-picks it, no boot picker to navigate).

    --image MUST be that standalone fusion (delta-artless + Red.gb + the trimmed
    1-mon Red.sav), not any corpus image -- a full-party save cannot reproduce this.

    RECIPE (found live, not guessed -- a probe run corrected both the brief's assumed
    tap count and the header text): info screen (auto-picked) -> A -> box grid (BOX1,
    cursor slot 0) -> L,L (TWO presses -- the first L only finishes the box grid's
    own initial paint/settle, still on BOX1; the SECOND L is the real wrap to the
    party pseudo-box) -> header reads "GB PARTY 1/6" (the "/6" is the party's fixed
    CAPACITY, gb_edit.h's own party-slot ceiling, not this save's trimmed count --
    the brief's guessed "1/1" was wrong), cursor already ON the one remaining mon
    (MEW), no separate DOWN needed (unlike a multi-box grid's title row, the party
    pseudo-box's own cursor lands directly on slot 0 when only one slot is
    occupied) -> A opens its mon menu. Expected rows, Gen-1 occupied-mon order
    (source/pdna_main.c, k_gb_ops_gen1 pdna_gen12.c): VIEW/EDIT, LEGALITY, COPY,
    DUPLICATE, TO DAY-CARE, EXPORT .pk, CANCEL -- MOVE TO BOX (PARTY_FLOOR, #166's
    existing gate) and RELEASE (PARTY_FLOOR, #182's new gate) BOTH omitted; ONE grey
    reason line shows ("Can't lift: last mon" -- PDNA_GB_LIFT_WHY_FLOOR, gb_lift_why_
    status()'s own bucket for GBS_ERR_PARTY_FLOOR), which is move_why's own line
    (RELEASE's fix deliberately adds no second line, per the fix's own comment --
    the same reason, worded once)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b182_")
    print("== BACKLOG #182: RO_RELEASE's lift_why gate -- 1-mon party PARTY_FLOOR "
          "refuses both MOVE TO BOX and RELEASE, no shown-then-refused row ==")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # info screen -> box grid (BOX1)
    s.shot("00_box_grid", "b182: Red-1mon's box grid on entry (pre-settle: the box header band is not painted yet) -- the preview panel reads SLOWBRO, so the party trim left the storage boxes alone", claim=["SLOWBRO"])

    s.press_n("L", 2, settle=150)                            # box grid -> (settle) -> wraps to the party
    s.shot("01_party_pseudo_box", "b182: two L presses from BOX1 wrap to the party "
           "pseudo-box -- header reads 'GB PARTY 1/6' ('/6' is the party's fixed "
           "capacity, not the trimmed count) -- cursor already on the one remaining "
           "mon, MEW (Red.sav's own party slot 0, the only slot this fixture kept)",
           claim=["GB PARTY", "1/6"])  # split in two: gb_claims.find's bitmap match
           # on the combined string fails on this exact kerning (checked live --
           # "GB PARTY" alone and "1/6" alone both match; "GB PARTY 1/6" and
           # "PARTY 1/6" do not -- a spacing quirk between the Y/1 glyphs, not a
           # missing-text defect; the PNG shows the full string plainly by eye)

    s.tap("A", settle=gb_shots.BIG_SETTLE)                   # A on the one remaining party mon -> its menu
    s.shot("02_party_mon_menu", "b182 THE FIX: MEW's occupied-mon menu on a 1-mon "
           "party -- VIEW/EDIT, LEGALITY, COPY, DUPLICATE, TO DAY-CARE, EXPORT .pk, "
           "CANCEL, plus ONE grey reason line ('Can't lift: last mon'). MOVE TO BOX "
           "(#166) and RELEASE (#182) are BOTH absent -- gb_lift_why_hook -> "
           "gb_lift_why_bs -> gbs_can_delete returns GBS_ERR_PARTY_FLOOR for this "
           "slot, and this fix's own `release_why` local (mirroring move_why) hides "
           "the RELEASE row the same way `move_why` already hid MOVE TO BOX. Before "
           "this fix RELEASE would still be in this list, opening gb_release_hook's "
           "confirm dialog and only bouncing off gbs_delete's own PARTY_FLOOR "
           "refusal at the end.",
           claim=["VIEW / EDIT", "LEGALITY", "COPY", "DUPLICATE", "TO DAY-CARE",
                  "CANCEL", "Can't lift: last mon"],
           claim_absent=["MOVE TO BOX", "RELEASE"])

    s.tap("B", settle=100)
    s.shot("03_back_to_party_box", "b182: B backs out of the menu, party pseudo-box "
           "unchanged")
    return s


def run_s150_7_down_edge(core_mod, image_mod, rom_gold: Path, rom_red: Path,
                         out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-7: the DOWN edge -- a native "GBC1" Bank cell back into a Game
    Boy save of the SAME generation (the EXACT arm). Demonstrates the gesture from D-Q7:
    MOVE from the Bank -> DOWN off the Bank's bottom -> A on the GB grid.

    TWO images, both `tools/fuse_gb.py <pokedna-delta-artless.gba> <ROM> <SAV>` (a
    single-directory-entry fusion, NOT tools/fuse_sav.py -- this needs a GB session
    actually mounted so the Bank's bank_edge UP hop and DOWN-off-bottom land on the
    session's OWN box grid, not a Gen-3 PC): `rom_gold` = Gold.gbc+Gold.sav (Gen 2),
    `rom_red` = Red.gb+Red.sav (Gen 1). bank_plant.c's PDNA_DELTA-only box_load() hook
    plants the SAME seven directed cells into box 0 regardless of which generation's
    session opened it (source/bank_plant.c, box0: CHIKORITA at slot 0 (Gen 2), PIKACHU
    at slot 1 (Gen 1), an Egg, an item holder, the DMG chip, plus BACKLOG #150 S150-12
    decision 17's two COPY cells at slots 5-6) -- so the Gold session's own
    Bank and the Red session's own Bank both show the identical CHIKORITA cell at slot 0,
    which is what lets one image demonstrate EXACT (dropped on Gold, same gen) and the
    other demonstrate GB_BRIDGE (dropped on Red, different gen) off the SAME cell.

    Nav recipe, verified live against these exact fused images (settle counts found by
    hand, same posture as every other run_* function in this file):
      grid entry: `s.run(700); s.run(100)` -- boots straight into the box grid (#279: the S1 info
        page is gone; single-directory-entry fusion, gb_delta_pick_save()
        auto-picks the lone slot).
      grid -> Bank: UP x3 (cell -> title -> tabs -> the bank_edge hop, same three-press
        count run_s2_bank()'s own docstring already measured) THEN UP x4 more (the Bank
        opens with the cursor on the BOTTOM row -- "unless carrying", gb_bank_visit()'s
        own comment -- and box 0's seven planted cells (slots 0-6, S150-12 decision
        17's two COPY cells included) sit in row 0 (six cells, the box is 6 columns
        wide) plus one cell at row 1 col 0, four rows up).
      pick up CHIKORITA: A (whitelist menu, VIEW/MOVE/RELEASE/CANCEL) -> DOWN -> A
        (selects MOVE, starts the carry).
      Bank -> back onto the GB grid, STILL CARRYING: DOWN x5 (4 to walk down to the
        Bank's own bottom row, a 5th off its bottom edge -- pdna_box.c's own
        `else if (src->is_bank) { boxoam_exit(); return 5; }`, which gb_bank_visit()
        turns into a plain re-entry of the GB grid's own pdna_box() loop).
      landing: R switches storage boxes (both Gold and Red have several boxes at/near
        capacity on Guy's real cartridges -- R until a box with room is found is more
        robust than a hardcoded count, since the corpus is real save data whose exact
        fill level was never a design invariant); R x15 from box 1 reaches the GB PARTY
        pseudo-box (14 storage boxes + the party, Gen 2 -- Gen 1 has 12 + the party, so
        the party index differs and is walked to separately below for panel (c))."""
    gb_shots.assert_vehicle(rom_gold, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    gb_shots.assert_vehicle(rom_red, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    print("== BACKLOG #150 S150-7: the DOWN edge (EXACT arm) ==")
    UP_INTO_BANK = 3          # grid -> title -> tabs -> the bank_edge hop
    UP_TO_ROW0 = 4            # Bank's own bottom row -> row 0 (the planted cells)
    DOWN_OFF_BANK = 5         # row 0 -> Bank's own bottom row (4) -> off the bottom edge (1 more)

    def boot_to_grid(s: gb_shots.Session) -> None:
        s.run(700)                       # #279: lands on the grid by itself (no info page, no tap)
        s.run(100)

    def pick_up_chikorita(s: gb_shots.Session) -> None:
        s.press_n("UP", UP_INTO_BANK, settle=100)
        s.press_n("UP", UP_TO_ROW0, settle=60)
        s.tap("A", settle=150)                      # native-cell whitelist menu
        s.press_n("DOWN", 2, settle=60)                    # VIEW/EDIT -> LEGALITY -> MOVE
        s.tap("A", settle=150)                       # MOVE -> carrying
        s.press_n("DOWN", DOWN_OFF_BANK, settle=150)  # back on the GB's own grid, still carrying

    def find_room_and_drop(s: gb_shots.Session, max_boxes: int, label_room: str,
                           label_drop: str) -> None:
        """R through storage boxes until one shows room (`N/20` with N < 20), then A."""
        for _ in range(max_boxes):
            s.tap("R", settle=150)
        s.shot(label_room, "S150-7: carrying CHIKORITA, R-ed to a storage box with "
               "room (real cartridge data -- boxes are R-ed past, not assumed empty)")
        s.tap("A", settle=250)
        s.shot(label_drop, "S150-7: A to drop -- see this shot's own caller for what "
               "arm this is expected to reach")

    # ---- (a)+(d) Gen 2 exact: Gold, CHIKORITA -> a Gold storage box with room -----
    sg = gb_shots.Session(core_mod, image_mod, rom_gold, out_dir, "s150_7_gold_")
    boot_to_grid(sg)
    sg.shot("00_gold_grid", "S150-7: Gold's own box grid, freshly entered")
    pick_up_chikorita(sg)
    sg.shot("01_gold_carrying", "S150-7: carrying the Gen-2 CHIKORITA cell, back on "
            "Gold's own grid (box 1) -- xg_bank_down_arm will see cell_gen==dst_gen==2")
    # Gold's own boxes 1..12 were all found FULL (20/20) against Guy's real cartridge
    # on this exact recipe (a real-cartridge corpus, not a design invariant) -- R past
    # them to box 13 (17/20), confirmed live.
    for _ in range(13):
        sg.tap("R", settle=150)
    sg.shot("02_gold_box_with_room", "S150-7: R-ed off box 1 (full, 20/20 on this "
            "cartridge) to a box with room")
    sg.tap("A", settle=250)
    sg.shot("03_gold_confirm", "S150-7 (a): the EXACT arm's confirm line -- "
            "'MOVE TO THE GAME?' / the mon's own nickname -- NOT the 'STAYS IN THE "
            "BANK' deny; a same-generation storage-box drop reaches bank_down_exact() "
            "-> gb_accept_down_hook() -> the ONE confirm line (D11)")
    sg.tap("A", settle=300)
    sg.shot("04_gold_delta_wall", "S150-7 (d): A = yes -> gbs_insert() succeeds in RAM "
            "-> gb_persist(\"bank-down\") hits pdna_gen12.c's #ifdef PDNA_DELTA refusal "
            "(this build has no card to write) -- 'Edits are in-session only in the "
            "emulator build', the PDNA_DELTA wall every write path in this tree shows. "
            "Real hardware proves the actual write (XFER-C7, docs/HW-QUEUE.md)")
    sg.tap("A", settle=200)                          # dismiss the wall
    sg.tap("B", settle=200)                          # cancel the still-held carry
    # Recovery + the D12 proof: navigate back into the Bank and confirm slot 0 (the
    # Bank's own copy of CHIKORITA) is UNCHANGED -- accept_down's internal gb_persist()
    # returned false, so bank_down_exact()'s own `if (!ok) return BANK_DOWN_REFUSED;`
    # fired BEFORE the app_bank_clear_slots() consume ever ran (D7/D12: the consume is
    # the LAST thing that happens, strictly after a TRUE persist).
    for _ in range(13):
        sg.tap("L", settle=150)                       # back to box 1
    sg.press_n("UP", UP_INTO_BANK, settle=100)
    sg.press_n("UP", UP_TO_ROW0, settle=60)
    sg.shot("05_gold_bank_cell_still_there", "S150-7 (d): back in the Bank, slot 0 -- "
            "CHIKORITA is STILL a native cell here (BANK 1  7/30 -- BACKLOG #150 "
            "S150-12's decision 17 added two more planted COPY cells at slots 5/6, "
            "unchanged from this lane's own box0 plant) -- the visual proof that a "
            "refused persist consumes NOTHING: D12's ordering held")

    # ---- (c) the 10(c) party-full offer + picker, on a FRESH carry off the same Bank -
    sg2 = gb_shots.Session(core_mod, image_mod, rom_gold, out_dir, "s150_7_gold_party_")
    boot_to_grid(sg2)
    pick_up_chikorita(sg2)
    # Gold's own party is 6/6 (full) on Guy's real cartridge -- R to the GB PARTY
    # pseudo-box (the box past every storage box; 15 R presses from box 1 on this
    # 14-storage-box corpus, confirmed live).
    for _ in range(15):
        sg2.tap("R", settle=150)
    sg2.shot("06_gold_party_full", "S150-7 (c): carrying CHIKORITA, R-ed to the GB "
             "PARTY pseudo-box -- GB PARTY 6/6, full on this cartridge")
    sg2.tap("A", settle=250)
    sg2.shot("07_gold_partyfull_offer", "S150-7 (c) SS11.20 item 10(c): the party-full "
             "offer -- 'PARTY IS FULL / Send a party Pokemon to a box first?' -- NOT a "
             "bare refusal (Q5's own answer)")
    sg2.tap("A", settle=250)
    sg2.shot("08_gold_party_picker", "S150-7 (c) D9: gb_pick_party_slot() -- 'SEND "
             "WHICH PARTY MON?', rows are the six party members' own nicknames (ui_ptext, "
             "gb_pick_box's own chrome), footer 'U/D pick  A ok  B cancel'")
    # REVIEW F6: 250 frames caught this specific two-line ui_ptext_wrap() confirm still
    # mid-repaint on real hardware timing (found re-shooting this exact frame) -- 400
    # is what a probe against this build confirmed settles both lines fully.
    sg2.tap("A", settle=400)                          # pick the top row
    sg2.shot("09_gold_party_confirm", "S150-7 (c)+(D-Q2/D-Q3)+F6: the deposit ran "
             "RAM-only (gbs_move, freeing a party slot), THEN the CHIKORITA landing's "
             "own confirm -- TWO lines, UX parity with the box-destination confirm "
             "(frame 03): the mon's own name on line 1 ('CHIKORITA'), the Gen-2 "
             "per-generation stats note on line 2 ('New stats, full HP, healthy.') -- "
             "D-Q3's wording accepted as 'Recomputes stats.' (Gen 1) / 'New stats, "
             "full HP, healthy.' (Gen 2)")

    # ---- (b) GB_BRIDGE: the SAME CHIKORITA cell, dropped on Red (a DIFFERENT gen) ----
    sr = gb_shots.Session(core_mod, image_mod, rom_red, out_dir, "s150_7_red_")
    boot_to_grid(sr)
    # BACKLOG #198 item 2: boot_to_grid() lands on Red's own SAVE box grid (GB BOX1),
    # NOT the Bank -- bank_plant.c's box0 plant only exists inside pdna_bank_show(),
    # which pick_up_chikorita() below is what actually navigates into (UP_INTO_BANK
    # then UP_TO_ROW0). This shot is taken BEFORE that navigation, so it shows Red's
    # own real box 1 (20/20, this cartridge's own Gen-1 mons -- SLOWBRO et al, no
    # CHIKORITA anywhere), not the Bank plant; recaptioned to say so honestly rather
    # than moved after the navigation (moving it would erase the "freshly entered,
    # nothing carried yet" baseline the rest of this chain's captions lean on).
    sr.shot("10_red_grid", "S150-7 (b): Red's OWN PC box grid (GB BOX1, 20/20), "
            "freshly entered, cursor top-left -- this is Red's real save data (this "
            "cartridge's own Gen-1 mons), NOT the Bank -- the Bank's own box0 plant "
            "(bank_plant.c) is only shown once pick_up_chikorita() below navigates "
            "into pdna_bank_show()")
    pick_up_chikorita(sr)
    sr.shot("11_red_carrying", "S150-7 (b): carrying the Gen-2 CHIKORITA cell, back on "
            "Red's own (Gen-1) grid -- xg_bank_down_arm resolves GB_BRIDGE, not EXACT")
    sr.tap("A", settle=250)
    # 84a43b8 (source/pdna_box.c: the occupied refusal is scoped to XG_DOWN_ARM_GEN3
    # only) landed AFTER this docstring's own "S150-8 stub" note was written, and it
    # also lands after S150-8's own bank_down_convert_gb() replaced the old one-line
    # `return BANK_DOWN_REFUSED;` GB_BRIDGE case body this comment used to describe --
    # so this A no longer hits a stub refusal (and Red's GB BOX1 is 20/20, fully
    # occupied, on this real cartridge: the fixed !occupied scoping is why dispatch
    # still runs here). 59dd45c (merged-tree review F1/F2/F3/F5, landed AFTER this
    # shot was first captured on 84a43b8) fixed the bridge converting to the WRONG
    # generation (its own SOURCE gen, not the MOUNTED session's) -- with dst_gen
    # correctly Gen 1 now, xr_time_capsule_block() (source/xfer_rec.c:26-43) sees
    # CHIKORITA's dex (152) exceed gb_max_species(GB_GEN1) (151) and blocks it
    # BEFORE any loss screen renders, re-captured live to confirm (not assumed):
    # this is no longer a preview at all, it is one of the two time-capsule
    # refusals --s150-8-bridge (a separate, NEW chain in this same file) exercises
    # end to end on its own planted cell. This shot stays only to show S150-7's own
    # GB_BRIDGE arm reaches the SAME refusal Red's full box0 plant would always hit.
    sr.shot("12_red_no_gen1_form", "S150-7 (b), post-59dd45c: 'NO GEN 1 FORM / "
            "CHIKORITA: no Gen 1 form.' -- xr_time_capsule_block()'s species-floor "
            "check (dex 152 > gb_max_species(GB_GEN1)=151) fires before any loss "
            "screen, now that the bridge correctly targets dst_gen=GB_GEN1 (the "
            "59dd45c fix) instead of the pre-fix bug's own source generation")

    sg.taken += sg2.taken + sr.taken
    sg.skipped += sg2.skipped + sr.skipped
    return sg


def run_b142_tab_focus_arrival(core_mod, image_mod, rom_after: Path, rom_before: Path,
                                out_dir: Path) -> gb_shots.Session:
    """BACKLOG #142: a directional arrival into an is_bank grid used to focus tab 2
    (SAVE) instead of the grid's top-left cell. The one live site: pdna_box.c's
    entry-time "cursor-arrival hint" -- `else if (st == 1 && !s_holding)
    s_tab_focus = src->is_bank ? 2 : 1;` (before this lane's fix). `st == 1` is set
    by app_box_start_set(1), both of whose call sites are captioned "bank dropped
    off the bottom -> PC opens/tabs" (pdna_main.c:10006, pdna_gen12.c:4371) -- i.e.
    this branch only ever runs for the destination screen of a Bank hand-off, never
    for the real Bank itself. `src->is_bank` is true here ONLY for a Game Boy
    session's own box (pdna_gen12_source() -- the real PC's is_bank is false, so it
    always took the ": 1" PARTY-tab arm, unaffected by this lane). A GB grid's tab 1
    is the INERT "(BANK)" label (pdna_box.c's own draw_tab call), so the ternary's
    "? 2" arm parked the cursor on SAVE -- one A there calls boxoam_exit() and
    return 0, ending the whole Game Boy session (pdna_box.c ~3927, "SAVE -> exit
    (save prompt)").

    Gesture, verified live against these exact fused images (both
    `tools/fuse_gb.py <pokedna-delta-artless.gba> Gold.gbc Gold.sav`, ONE built
    from this lane's own fixed source, ONE from a scratch pre-fix build of the
    same commit the lane branched from -- same idiom as run_s150_7_down_edge's
    two-image A/B, just before/after code instead of two save files):
      grid entry: boot_to_gb_session(..., which="gold") -- single-ROM fusion, no
        picker, lands on Gold's own box grid, slot 0 = No.1 BULBASAUR (the same
        corpus run_s150_4_uplift already shot).
      grid -> the GB session's own linked Bank: UP x3 (cell -> title -> tabs ->
        the bank_edge hop) -- `from_hop` sets app_box_start_set(2), so the linked
        Bank opens on its OWN bottom row already (pdna_gen12.c:4368, mirrors
        run_s150_7_down_edge's own "Bank opens on the bottom row" finding).
      Bank -> back onto the GB grid: DOWN x1 -- already on the bottom row, so this
        one press is "off the bank bottom -> PC tabs" (pdna_box.c:4021), returning
        5; gb_bank_visit() turns that into app_box_start_set(1) (pdna_gen12.c:4371)
        and re-enters the GB source's own pdna_box() -- the exact `st == 1` arrival
        this lane's fix touches.
    """
    gb_shots.assert_vehicle(rom_after, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    gb_shots.assert_vehicle(rom_before, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    print("== BACKLOG #142: is_bank grid tab-focus arrival ==")
    UP_INTO_BANK = 3     # cell -> title -> tabs -> the bank_edge hop
    DOWN_OFF_BANK = 1    # the linked Bank opens on its own bottom row already

    def arrive(s: gb_shots.Session, rom: Path) -> None:
        boot_to_gb_session(s, rom, which="gold")
        s.press_n("UP", UP_INTO_BANK, settle=100)
        s.press_n("DOWN", DOWN_OFF_BANK, settle=150)

    # ---- BEFORE: the merged main's arrival lands on SAVE -----------------------
    sb = gb_shots.Session(core_mod, image_mod, rom_before, out_dir, "b142_before_")
    arrive(sb, rom_before)
    sb.shot("00_before_arrival", "BACKLOG #142 (before the fix): back on Gold's own "
            "box grid after the Bank dropped off its bottom -- the top-right tab "
            "reads SAVE, highlighted, not the grid")
    sb.tap("A", settle=200)
    sb.shot("01_before_stray_a", "BACKLOG #142 (before the fix): one A on that "
            "arrival hit the SAVE tab -- boxoam_exit() ran and the box grid is gone; "
            "this save's own save-exit flow now shows its 'NOT TRANSFERABLE' report "
            "(items held on mons this generation can't take along), exactly the "
            "unintended save-and-exit the reported defect describes, not a deliberate "
            "SAVE press")

    # ---- AFTER: the fix lands on the top-left cell instead ---------------------
    sa = gb_shots.Session(core_mod, image_mod, rom_after, out_dir, "b142_after_")
    arrive(sa, rom_after)
    sa.shot("02_after_arrival", "BACKLOG #142 (after the fix): the same arrival -- "
            "s_tab_focus stays -1 (unset by this branch now), cur stays 0 -- the "
            "hand cursor sits on the grid's top-left cell, no tab highlighted")
    sa.tap("A", settle=200)
    sa.shot("03_after_safe_a", "BACKLOG #142 (after the fix): the same stray A now "
            "opens the ordinary cell-0 action menu instead of ending the session -- "
            "the tabs are still reachable, just by pressing UP first, same as any "
            "other grid visit")

    sb.taken += sa.taken
    sb.skipped += sa.skipped
    return sb
def run_s150_9_merge_screen(core_mod, image_mod, rom_emerald: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-9: the shared per-field MERGE screen (app_xfer_merge_screen,
    source/pdna_main.c), both the real-rows case and decision 7's "nothing changed"
    skip, plus decision 8's RESTORED/PENDING refusals -- all four reachable only
    because of decision 12's planted-ledger read shim (source/bank_plant.c/
    xfer_io.c): this vehicle's flash chip has no writable FAT at all, confirmed live
    by an earlier lane's own S150-8 chain (every real DOWN ledger write hits
    "SIDECAR FOLDER / Nothing transferred." at the f_mkdir step) -- there is no way
    to reach a REAL ledger entry on the delta without planting one directly in RAM.

    `rom_emerald` MUST be `tools/fuse_sav.py <pokedna-delta-artless.gba> Emerald.sav`
    (a plain Gen-3 fusion, no --gb -- same vehicle shape as --s2-bank-control/--s150-8).
    bank_plant_xfer_seed_all()'s PC-storage mount hook (source/pdna_main.c) plants
    FOUR Gen-3 records into THIS session's own PC box 0 (storage index 0 -- the
    box shown by default at boot; PokeDNA's own box title reads "1:<name>", the
    LEADING digit is the 1-based storage index, everything after the colon is the
    save's own custom box name -- verified live, not assumed, after an initial
    misread of "1:5.Unp09n" as box "5") at slots 29/28/27/26:
      29 (idx 0): CLAIMED, ALTERED (written_level -3, a moves[1] swap) -- the main
          chain, a real LEVEL/MOVES row pair.
      28 (idx 1): CLAIMED, unaltered -- decision 7's "nothing changed" skip.
      27 (idx 2): XR_STATE_RESTORED -- decision 8's ALREADY RESTORED refusal.
      26 (idx 3): XR_STATE_PENDING -- decision 8's SAVE FIRST refusal.
    All four are Gen-2 CHIKORITA (species 152), distinguishable on screen only by
    their level (12/14/15/18) -- the info panel after grabbing each cell is this
    chain's own confirmation of which planted slot is currently held.

    Nav recipe, verified live against this exact fused image (probe screenshots
    under /tmp/s150-9-trace/s150_9*_*.png, not guessed -- an EARLIER attempt tried
    UP once off the PC grid's top row expecting a "(BANK)" tab (by analogy with
    S150-8's own PARTY-tab note) and found none: a Gen-3 PC source's middle tab is
    permanently labelled PARTY, never becomes "(BANK)". The real route is
    pdna_box()'s own `r == 4` return code ("up past the PC tabs -> Bank, cursor
    from below", source/pdna_main.c ~:10568) -- ONE MORE UP past the tab row itself,
    not a tab cycle):
      grid entry: box 1 (storage index 0) is already on screen at boot -- cursor
        starts at slot 0 (top-left).
      to slot N: DOWN x(N//6), RIGHT x(N%6) -- e.g. slot 29 is DOWNx4, RIGHTx5.
      pick up: A (the cell's own menu -- VIEW/EDIT, ITEM, LEGALITY, MOVE, COPY,
        DUPLICATE, TO DAY-CARE, EXPORT .pk, RELEASE) -> DOWN x3 (-> MOVE) -> A
        (starts the carry, footer becomes "A drop  B cancel").
      into the Bank, STILL CARRYING: UP x6 -- 4 to reach row 0 (box 1 has 5 rows,
        cursor starts on row 4 after grabbing slot 29; fewer UPs are needed from a
        higher slot, this function always grabs a bottom-row slot so 4 is exact),
        1 more off the grid onto the tab row (lands on PKMN DATA, the leftmost),
        1 more off the TOP of the tab row -> `pdna_box()` returns 4 -> the caller
        opens `pdna_bank_show()` directly, still carrying. Box 1 of the Bank shows
        bank_plant_box0()'s own five cells (CHI/PIK/EGG/CHI/DMG); the cursor lands
        past them on an empty cell -- no further navigation needed to drop.
      drop: A. The merge screen (when it draws): U/D moves the row cursor, A flips
        KEEP/TAKE, START applies, B cancels back to the Bank grid still carrying."""
    gb_shots.assert_vehicle(rom_emerald, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    print("== BACKLOG #150 S150-9: the per-field MERGE screen ==")

    def goto_slot_and_grab(s: gb_shots.Session, slot: int) -> None:
        row, col = divmod(slot, 6)
        s.press_n("DOWN", row, settle=100)
        s.press_n("RIGHT", col, settle=100)
        s.tap("A", settle=200)              # the cell's own menu
        s.press_n("DOWN", 3, settle=80)     # -> MOVE
        s.tap("A", settle=200)              # pick up -> carrying

    def into_bank(s: gb_shots.Session) -> None:
        s.press_n("UP", 6, settle=100)      # row0, tab row, off the top -> the Bank

    s = gb_shots.Session(core_mod, image_mod, rom_emerald, out_dir, "s150_9_")
    s.run(700)
    s.shot("00_boot", "S150-9: Emerald boots into the PC box view, box 1 (storage "
           "index 0) shown by default -- the last row's four planted CHIKORITA "
           "cells (slots 26-29) are visible at the bottom-right")
    s.press_n("DOWN", 4, settle=100)
    s.press_n("RIGHT", 5, settle=100)
    s.shot("01_cursor_slot29", "S150-9: cursor moved to slot 29 (DOWNx4, RIGHTx5) "
           "-- info panel shows No.152 CHIKORITA Lv12, the main-chain planted cell")
    s.tap("A", settle=200)
    s.shot("02_menu", "S150-9: A opens the cell's menu (VIEW/EDIT, ITEM, "
           "LEGALITY, MOVE, COPY, DUPLICATE, TO DAY-CARE, EXPORT .pk, RELEASE)")
    s.press_n("DOWN", 3, settle=80)
    s.shot("03_on_move", "S150-9: DOWNx3 -> cursor on MOVE")
    s.tap("A", settle=200)
    s.shot("04_carrying", "S150-9: A -> picked up, carrying (footer 'A drop B "
           "cancel')")
    into_bank(s)
    s.shot("05_in_bank", "S150-9: UPx6 (off the grid, past the tab row, off the "
           "top) -> the Bank itself, still carrying -- box 1 shows "
           "bank_plant_box0's own SEVEN cells (CHI/PIK/EGG/CHI/DMG/CHI/CHI -- "
           "S150-12 review decision 17, merged after this lane's first pass, "
           "added two COPY cells at slots 5/6; was five cells pre-merge), "
           "cursor past them on an empty cell")
    s.tap("A", settle=300)
    s.shot("06_merge_screen", "S150-9: A to drop -> the per-field MERGE screen -- "
           "'BACK TO ITS ORIGINAL' / 'Level 9 > 12  KEEP' (cursor here) / "
           "'Moves changed  KEEP' / 'A flip  START apply  B cancel'",
           claim=["BACK TO ITS ORIGINAL", "KEEP"])  # BACKLOG #184 retrofit:
           # pdna_layout.h's PDNA_XFERRESTORE_TITLE / PDNA_XFERMERGE_KEEP literals
    s.tap("A", settle=200)
    s.shot("07_level_take", "S150-9: A flips the cursor row -- LEVEL now TAKE")
    s.tap("DOWN", settle=150)
    s.shot("08_cursor_moves", "S150-9: DOWN moves the cursor to the MOVES row")
    s.tap("A", settle=200)
    s.shot("09_moves_take", "S150-9: A flips MOVES to TAKE (both rows now TAKE)")
    s.tap("A", settle=200)
    s.shot("10_moves_keep_again", "S150-9: A again flips MOVES back to KEEP")
    s.tap("B", settle=250)
    s.shot("11_cancel_still_holding", "S150-9: B cancels the screen -- back on "
           "the Bank grid, STILL carrying (footer 'A drop B cancel'), nothing "
           "applied")
    s.tap("A", settle=300)
    s.shot("12_redrop_menu", "S150-9: A again on the same empty cell -- the "
           "screen reappears")
    s.shot("13_both_keep_again", "S150-9: both rows read KEEP again -- toggle "
           "state is NOT remembered across a cancel", allow_same=True)
    s.tap("A", settle=200)
    s.shot("14_level_take_again", "S150-9: A -- LEVEL -> TAKE (MOVES stays KEEP)")
    s.tap("START", settle=300)
    s.shot("15_write_result", "S150-9: START applies (accept=LEVEL only) -- "
           "'TRANSFER RECORD UNREADABLE / Nothing was moved.' (PDNA_XFERREC_*). "
           "The real mechanism (review D6, corrected from an earlier guess): "
           "pc_bank_restore_up's own pdna_bank_next_serial() call fails (it "
           "writes bank.meta -- no writable FAT on this vehicle at all), so "
           "`serial == 0` and pc_bank_restore_up returns -1 BEFORE "
           "bank_restore_from_entry is ever called -- the restore itself never "
           "ran here. drop_held's own rc<0 branch shows this same message for "
           "any negative pc_bank_restore_up return (source/pdna_box.c ~:1633-1642). "
           "Hardware-owed (docs/HW-QUEUE.md): whether the merge lands correctly "
           "when a real card CAN allocate a serial is untested here.",
           claim=["TRANSFER RECORD UNREADABLE"])  # BACKLOG #184 retrofit:
           # pdna_layout.h's PDNA_XFERREC_TITLE literal

    # ---- decision 7: slot 28 -- the "nothing changed" skip -----------------------
    s2 = gb_shots.Session(core_mod, image_mod, rom_emerald, out_dir, "s150_9b_")
    s2.run(700)
    goto_slot_and_grab(s2, 28)
    s2.shot("00_carrying_slot28", "S150-9 decision 7: carrying the slot-28 "
            "planted cell (CLAIMED, unaltered -- byte-identical to its own "
            "ledger baseline)")
    into_bank(s2)
    s2.tap("A", settle=300)
    s2.shot("01_no_screen", "S150-9 decision 7: A to drop -- NO merge screen "
            "(nothing changed abroad, decision 7's own rule; app_xfer_merge_screen "
            "returns true with *accept=0 without drawing), straight to the SAME "
            "pdna_bank_next_serial() failure frame 15 reaches (review D6)")

    # ---- decision 8: slot 27 -- XR_STATE_RESTORED, the ALREADY RESTORED refusal --
    s3 = gb_shots.Session(core_mod, image_mod, rom_emerald, out_dir, "s150_9c_")
    s3.run(700)
    goto_slot_and_grab(s3, 27)
    s3.shot("00_carrying_slot27", "S150-9 decision 8: carrying the slot-27 "
            "planted cell (its ledger entry is already XR_STATE_RESTORED)")
    into_bank(s3)
    s3.tap("A", settle=300)
    s3.shot("01_already_restored", "S150-9 decision 8: A to drop -- 'ALREADY "
            "RESTORED / The Bank has its original. / Release this copy "
            "instead.' (PDNA_XFERDUP_*), the state refusal BEFORE the screen -- "
            "nothing written, still holding",
            claim=["ALREADY RESTORED"])  # BACKLOG #184 retrofit:
            # pdna_layout.h's PDNA_XFERDUP_TITLE literal
    s3.tap("A", settle=200)
    s3.shot("02_still_holding", "S150-9 decision 8: dismiss -- still carrying "
            "the slot-27 cell, footer 'A drop B cancel'")

    # ---- decision 8: slot 26 -- XR_STATE_PENDING, the SAVE FIRST refusal ---------
    s4 = gb_shots.Session(core_mod, image_mod, rom_emerald, out_dir, "s150_9d_")
    s4.run(700)
    goto_slot_and_grab(s4, 26)
    s4.shot("00_carrying_slot26", "S150-9 decision 8: carrying the slot-26 "
            "planted cell (its ledger entry is XR_STATE_PENDING)")
    into_bank(s4)
    s4.tap("A", settle=300)
    s4.shot("01_save_first", "S150-9 decision 8: A to drop -- 'SAVE FIRST / One "
            "transfer is waiting for / the game save. START > SAVE.' "
            "(PDNA_XFER_SAVEFIRST_*) -- nothing written, still holding",
            claim=["SAVE FIRST"])  # BACKLOG #184 retrofit:
            # pdna_layout.h's PDNA_XFER_SAVEFIRST_TITLE literal
    s4.tap("A", settle=200)
    s4.shot("02_still_holding", "S150-9 decision 8: dismiss -- still carrying "
            "the slot-26 cell, footer 'A drop B cancel'")

    # Fold the three follow-on sessions' shots into the first session's own lists
    # so the caller's manifest/exit-code accounting sees all four.
    s.taken += s2.taken + s3.taken + s4.taken
    s.skipped += s2.skipped + s3.skipped + s4.skipped
    return s


def run_s150_9_site2(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #209: site 2 of the restore -- gb_lift_restore (source/pdna_gen12.c),
    a GB-grid lift of a mon whose ledger entry has a NATIVE home. Until this lane,
    gb_has_sidecar's own f_stat pre-check (xr_path_for_key) always missed on the
    delta (no FAT at all), so gb_lift_restore had NEVER executed on any vehicle --
    the whole point of this chain is to PROVE it now does.

    `rom` MUST be `make delta-gb`'s own combined image (Emerald.sav + Red/Gold/
    Crystal) -- the SAME vehicle --s150-4 uses. boot_to_gb_session(which="red")
    takes gb_delta_boot_pick()'s "PICK A SAVE" -> Red.sav row (this image's flash
    is never blank, Emerald.sav seeds it, so the blank-flash GB fork's own seed call
    never runs -- source/pdna_main.c's OTHER seed call site, right before THIS
    boot path's own pdna_gen12_show_image(), is the one that actually fires),
    landing on Red's box grid with box 0 slot 0 seeded by bank_plant_site2_seed()
    from whatever that exact cell decodes to at mount time.

    b199 review D2 (repair, BACKLOG #199 chain D moved the call site): this chain
    used to assert the restore screen / origin prompt appeared at GRAB time (SEL,
    A on the seeded cell). Lane b199's own chain D moved gb_lift_pack's call from
    start_carry (grab) to drop_held_up (the one drop that actually needs the
    Bank's price, source/pdna_gen12.c's own comment on gb_lift_pack: "the call
    moved from grab time... to the one drop that actually needs the Bank's
    price") -- so A on the seeded cell now just HOLDS (footer "A drop  B
    cancel", no screen at all, the SAME footer an ordinary unseeded grab shows),
    and gb_has_sidecar()'s branch only fires once the carried mon is DROPPED on
    an empty Bank cell. Re-derived live against this exact vehicle (not assumed):
    SEL -> A (grab, no screen) -> UP, UP (the bank_edge hop, still carrying,
    landing on BANK 1 with the cursor already on the first empty cell -- slot 7,
    right after bank_plant_box0()'s seven planted cells) -> A on that cell is
    the drop that finally calls gb_lift_pack() -> gb_has_sidecar() finds the
    seeded entry -> gb_lift_restore() runs (decision 10: "the origin prompt is
    SKIPPED -- the home cell carries its own origin_game already") -- so THIS
    drop, not the grab, is where the restore/merge screen or its absence is the
    proof.

    xr_merge_down_gb_sel's own probe reports DIFFERING rows against the seeded
    home (BACKLOG #206/#209 review D2: the seed's home cell is a CHANGED copy of
    the live mon -- 5 levels lower and renamed -- so gb_lift_restore's probe reads
    level_changed=1/renamed=1 and the screen DRAWS instead of skipping) --
    app_xfer_merge_screen ("BACK TO ITS ORIGINAL") shows two toggle rows, both
    defaulting to KEEP -- found live: "Level 95 > 100" / "Nickname changed", both
    KEEP.

    B ON THIS SCREEN, POST-D5 (re-verified live after D5 landed in this same
    lane): gb_lift_restore() returns -2 on a plain decline (its own "B: nothing
    spent" comment, now XG_LIFT_CANCELLED via gb_lift_pack -- pdna_box.h), which
    drop_held_up()'s tri-state check treats as "already explained on screen" and
    shows NOTHING further -- straight back to the Bank grid, still holding, no
    dialog at all. BEFORE D5 this exact B press showed PDNA_XFER_LIFT_REFUSED_
    TITLE -- "NOT MOVED TO THE BANK / This Pokemon could not be packed for the
    Bank." -- on a plain decline, a real bug (an error dialog for a user's own
    B press); this chain's own frame 05 is now the live proof it is fixed. A
    re-grab needs no fresh SEL/A, just another A on the cell (found live:
    pressing A on the same cell again reopens the identical merge screen,
    s_holding never cleared by a CANCELLED return any more than a FAILED one
    did). START from the merge screen (both rows left at KEEP, nothing
    accepted) reaches pdna_bank_next_serial(), which ALSO fails on this vehicle
    (no writable FAT, the same wall run_s150_4_uplift's own chain documents) --
    a GENUINE unreported failure (XG_LIFT_FAILED, not CANCELLED), so START DOES
    still show "NOT MOVED TO THE BANK" -- this vehicle can now tell "the user
    declined" (silent) apart from "the write failed" (a real dialog) on screen,
    D5's whole point, proven by frames 05 and 07 disagreeing on purpose.

    A landed, persisted native cell (bank.meta writable) is hardware-only from
    here, same as run_s150_4_uplift's own chain -- not faked with a pre-planted
    stand-in cell."""
    gb_shots.assert_vehicle(rom, "ART")  # BACKLOG #255 review: this docstring says `make delta-gb`, which is PDNA_TARGET=delta
                                    # with NO PDNA_ARTLESS=1 (Makefile:567) -- the FULL-ART delta. Asserting
                                    # ARTLESS here hard-refused the very build the chain prescribes.
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_9_site2_")
    print("== BACKLOG #209: site 2 of the restore (gb_lift_restore) ==")
    boot_to_gb_session(s, rom, which="red")
    s.shot("00_red_box_grid", "s150-9-site2: Red's box grid, freshly entered -- "
           "cursor on slot 0, the SEEDED cell (bank_plant_site2_seed() keyed this "
           "exact mon's identity at mount time), footer 'A menu  SEL  L/R  B'")

    s.tap("SEL", settle=100)
    s.shot("01_cm_move", "s150-9-site2: SELECT cycles the cursor mode to MOVE -- "
           "footer 'MOVE  A grab  hold=set'")

    s.tap("A", settle=150)
    s.shot("02_grabbed_no_screen", "s150-9-site2 (b199 D2 repair): A grabs slot 0 "
           "-- footer 'A drop  B cancel', NO screen at all -- BACKLOG #199 chain D "
           "moved gb_lift_pack (and therefore gb_has_sidecar's restore-vs-fresh "
           "branch) off the grab entirely; start_carry() is now a plain memcpy for "
           "every scope, seeded or not.",
           claim=["A drop  B cancel"])

    s.tap("UP", settle=gb_shots.BIG_SETTLE); s.run(60)
    s.tap("UP", settle=150); s.run(100)
    s.shot("03_bank_hop", "s150-9-site2 (b199 D2 repair): 2xUP, carrying -- row0 -> "
           "tab focus -> the bank_edge hop (`return 4`) lands on BANK 1, STILL "
           "carrying, cursor already on the first EMPTY cell (slot 7, right after "
           "bank_plant_box0()'s seven planted native cells) -- the next A is the "
           "drop that finally calls gb_lift_pack().")

    s.tap("A", settle=gb_shots.BIG_SETTLE); s.run(100)
    s.shot("04_merge_screen", "s150-9-site2 (b199 D2 repair): A on the empty Bank "
           "cell -- THIS is the drop gb_lift_pack() now runs on. gb_has_sidecar() "
           "finds the seeded entry -> gb_lift_restore() runs (decision 10: the "
           "origin prompt is SKIPPED, the home cell carries its own origin_game) "
           "-> xr_merge_down_gb_sel's probe found two differing rows against the "
           "seeded home -> app_xfer_merge_screen draws straight away, 'BACK TO ITS "
           "ORIGINAL' with a Level and a Nickname row, both KEEP, footer 'A flip  "
           "START apply  B cancel'.",
           claim=["BACK TO ITS ORIGINAL", "Level", "Nickname", "KEEP"])

    s.tap("B", settle=gb_shots.BIG_SETTLE); s.run(100)
    s.shot("05_cancel_silent", "s150-9-site2 (b199 review D5 fix, re-verified after "
           "D5 landed in this same lane): B cancels the merge screen -- "
           "gb_lift_restore()'s B-decline now returns -2 (XG_LIFT_CANCELLED via "
           "gb_lift_pack), which drop_held_up() treats as 'already explained on "
           "screen' and shows NOTHING further -- straight back to the Bank grid, "
           "STILL HOLDING the same mon (footer 'A drop  B cancel'), no dialog at "
           "all. BEFORE D5 this exact B press showed 'NOT MOVED TO THE BANK / "
           "This Pokemon could not be packed for the Bank.' on a PLAIN DECLINE -- "
           "the bug D5 fixes; this frame is the live proof it is fixed.",
           claim=["A drop  B cancel"],
           claim_absent=["NOT MOVED TO THE BANK", "This Pokemon could not be"])

    s.tap("A", settle=gb_shots.BIG_SETTLE); s.run(100)
    s.shot("06_regrab_merge_screen", "s150-9-site2 (b199 D2 repair): A on the "
           "SAME cell again -- the identical merge screen redraws (pixel-"
           "identical to frame 04: nothing was applied or persisted by the B "
           "cancel above, so gb_lift_pack() runs the exact same probe again).",
           allow_same=True,
           claim=["BACK TO ITS ORIGINAL", "Level", "Nickname", "KEEP"])

    s.tap("START", settle=gb_shots.BIG_SETTLE); s.run(150)
    s.shot("07_start_refused", "s150-9-site2 (b199 review D5 fix, re-verified): "
           "START confirms the merge screen (both rows left at KEEP, nothing "
           "accepted) -- gb_lift_restore() then reaches pdna_bank_next_serial(), "
           "which fails on this vehicle (no writable FAT, the SAME wall "
           "run_s150_4_uplift's own chain documents) -- a GENUINE unreported "
           "failure (XG_LIFT_FAILED), unlike frame 05's plain decline, so "
           "drop_held_up() DOES show 'NOT MOVED TO THE BANK / This Pokemon "
           "could not be packed for the Bank.' here -- D5's whole point: this "
           "vehicle can now tell 'the user declined' (frame 05, silent) apart "
           "from 'the write failed' (this frame, a real dialog) on screen.",
           claim=["NOT MOVED TO THE BANK", "This Pokemon could not be",
                  "packed for the Bank."])

    # ---- A/B proof, SAME image/session shape, ONE cell over: slot 1 (unseeded) --
    # DOES show the origin prompt, exactly where slot 0 (seeded) does not -- the
    # direct demonstration that gb_has_sidecar()/xr_path_for_key's own PDNA_DELTA
    # shim is what changed frame 04's outcome above, not some vehicle-wide inability
    # to ever draw the prompt at all (a real risk to rule out on a build with no
    # writable FAT anywhere near this path). Relocated to the SAME bank-drop site
    # as the main chain above (b199 D2 repair) -- the grab itself shows nothing for
    # either slot now.
    s2 = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_9_site2_cmp_")
    boot_to_gb_session(s2, rom, which="red")
    s2.tap("RIGHT", settle=100)
    s2.shot("00_slot1", "s150-9-site2 A/B: cursor moved one cell right, to slot 1 "
            "-- UNSEEDED (bank_plant_site2_seed only ever seeds box 0 slot 0)")
    s2.tap("SEL", settle=100)
    s2.tap("A", settle=150)
    s2.shot("01_grabbed_no_screen", "s150-9-site2 A/B (b199 D2 repair): A grabs "
            "slot 1 -- footer 'A drop  B cancel', no screen, same as slot 0's own "
            "grab -- the grab itself no longer distinguishes seeded from unseeded.",
            claim=["A drop  B cancel"])
    s2.tap("UP", settle=gb_shots.BIG_SETTLE); s2.run(60)
    s2.tap("UP", settle=150); s2.run(100)
    s2.shot("02_bank_hop", "s150-9-site2 A/B (b199 D2 repair): 2xUP, carrying -- "
            "the same bank_edge hop, landing on the same first-empty Bank cell.")
    s2.tap("A", settle=gb_shots.BIG_SETTLE); s2.run(100)
    s2.shot("03_prompt_shows", "s150-9-site2 A/B (b199 D2 repair): A on the empty "
            "Bank cell -- 'WHICH GAME IS THIS?' RED (selected) / BLUE / YELLOW "
            "DOES draw here, on the exact same image/session/gesture that skipped "
            "it for slot 0 -- gb_has_sidecar() answers false for this ordinary, "
            "unseeded cell, so gb_lift_pack() takes the normal (non-restore) path "
            "instead. This is the direct proof that slot 0's missing prompt is the "
            "seed/shim working, not a vehicle-wide inability to ever draw this "
            "screen -- relocated to the drop site, same as the main chain above.",
            claim=["WHICH GAME IS THIS", "RED", "YELLOW"])

    s.taken += s2.taken
    s.skipped += s2.skipped
    return s


def run_s150_8_gen3_arm(core_mod, image_mod, rom_emerald: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-8: the CONVERTING DOWN edge's GEN3 arm -- a native "GBC1"
    Bank cell converts into a real Gen-3 record and lands in the CURRENT Gen-3
    session's own PC box (source/bank_down_convert.c bank_down_convert_gen3() ->
    source/pdna_gen12.c gb_bank_down_gen3()).

    `rom_emerald` = `tools/fuse_sav.py <pokedna-delta-artless.gba> <Emerald.sav>` (a
    PLAIN Gen-3 fusion, no --gb -- the SAME vehicle shape run_s2_bank_control() above
    already uses for an ordinary Gen-3 Bank visit). bank_plant.c's box_load() hook is
    generation-agnostic (its own comment, quoted in run_s150_7_down_edge's docstring
    above): the Bank's box 0 plants the identical seven native cells regardless of
    which generation's session opened it, so this Emerald session's own Bank shows
    slot 3 (0-indexed) holding a CHIKORITA carrying item id 19 (ESCAPE ROPE) --
    the ONE planted cell with BOTH an item (for the loss screen's "Item: ... travels"
    row, per this lane's own brief) and, since it is a base-form species at any
    level, no evolution-floor violation. NONE of bank_plant_box0()'s seven cells
    (Gen-2 CHIKORITA plain/egg/item-holding, Gen-1 PIKACHU, the DMG glitch chip, and
    S150-12's two COPY CHIKORITAs at slots 5-6) is an EVOLVED species below
    pk_evo_floor() -- Gen 1/2 base forms are legal at any level -- so the brief's
    own "(pick such a cell if one exists; else say none is planted)" instruction
    applies: none is planted, and the KEEP AS IS / MAKE LEGAL
    screen (source/pdna_gen12.c gb_bank_down_gen3():2765-2780) is never reached by
    this chain.

    Nav recipe, verified live against this exact fused image (probe screenshots
    under /tmp/s150-78-trace/s150-8/d*_*.png, not guessed):
      Bank entry: `s.tap("START", settle=80); s.tap("DOWN", settle=60); s.tap("A",
        settle=150)` -- the SAME three taps run_s2_bank_control()'s own "02_bank"
        uses (Party -> Bank is one DOWN in the nav list, then A opens
        pdna_bank_show()). Box 0's cursor starts on slot 0 (plain CHIKORITA); RIGHT
        x3 reaches slot 3 (the item-holding CHIKORITA).
      pick up: A (native-cell whitelist: VIEW/EDIT, MOVE, RELEASE, CANCEL) -> DOWN
        (VIEW/EDIT -> MOVE) -> A (starts the carry).
      Bank -> PC, still carrying: DOWN x5 (row 0 down to the Bank's own bottom row,
        a 5th off its bottom edge) -- IDENTICAL to run_s150_7_down_edge's own
        DOWN_OFF_BANK=5 (pdna_box.c's `else if (src->is_bank) { boxoam_exit();
        return 5; }` does not care which generation's session is watching).
      landing: this Emerald save's PC box index (whatever `records()` reloads to
        after the Bank exit -- this corpus lands on "5:Unp09n 30/30", a FULL box,
        30/30) puts the cursor on an OCCUPIED cell by default -- used AS-IS for the
        occupied-cell refusal (no navigation needed, matching the brief's own "drop
        on an OCCUPIED PC cell" ask). R x10 from there reaches "11:Qo 0/30", this
        corpus's own completely EMPTY box (real cartridge data, found live, same
        R-until-room convention run_s150_7_down_edge's find_room_and_drop() uses) --
        cursor lands on an empty cell there with NO further navigation, since the
        whole box is empty.
      The party arm (BACKLOG #174, S150-8c) is a SEPARATE function now --
      run_s150_8_party_vsd(), below -- since proving a real landing needs a real SD
      write this vehicle's flash chip cannot serve (see this function's own last
      frames, the SIDECAR FOLDER wall)."""
    gb_shots.assert_vehicle(rom_emerald, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    print("== BACKLOG #150 S150-8: the DOWN edge (GEN3 arm) ==")
    DOWN_OFF_BANK = 5   # same constant as run_s150_7_down_edge -- generation-agnostic

    def enter_bank(s: gb_shots.Session) -> None:
        s.run(700)
        s.tap("START", settle=80)
        s.tap("DOWN", settle=60)
        s.tap("A", settle=150)

    def pick_up_item_cell(s: gb_shots.Session) -> None:
        s.press_n("RIGHT", 3, settle=100)             # slot 0 -> slot 3 (item-holding CHIKORITA)
        s.tap("A", settle=150)                        # native-cell whitelist menu
        s.press_n("DOWN", 2, settle=60)                      # VIEW/EDIT -> LEGALITY -> MOVE
        s.tap("A", settle=150)                        # MOVE -> carrying
        s.press_n("DOWN", DOWN_OFF_BANK, settle=150)  # back on the Emerald PC grid, still carrying

    s = gb_shots.Session(core_mod, image_mod, rom_emerald, out_dir, "s150_8_")
    enter_bank(s)
    s.shot("00_bank", "S150-8: the Bank, opened from an ordinary Emerald session -- "
           "box 0 shows bank_plant.c's seven generation-agnostic native cells "
           "(CHI/PIK/EGG/CHI/DMG at slots 0-4, plus S150-12's two COPY CHIKORITAs "
           "at slots 5-6); the cursor starts on slot 0")
    pick_up_item_cell(s)
    s.shot("01_landed_occupied", "S150-8: carrying the item-holding CHIKORITA cell, "
           "landed on this save's own PC box 1 (named '5.Unp09n', 30/30, full on this cartridge) -- "
           "cursor sits on an OCCUPIED cell by default, used as-is for the "
           "occupied-cell refusal below")

    # ---- drop on an OCCUPIED PC cell: refused BEFORE dispatch (review F7) --------
    s.tap("A", settle=250)
    s.shot("02_occupied_refusal", "S150-8: A on the occupied cell -- 'STAYS IN THE "
           "BANK / This Game Boy Pokemon can only move inside the Bank.' -- "
           "pdna_box.c's `!(arm == XG_DOWN_ARM_GEN3 && occupied)` gate (84a43b8) "
           "refuses BEFORE bank_down_dispatch/gb_bank_down_gen3 ever runs")
    s.tap("A", settle=200)                            # dismiss
    s.shot("03_still_holding_after_occupied", "S150-8: still carrying after the "
           "occupied refusal -- nothing was touched (D12's ordering: a refusal "
           "before dispatch consumes nothing, same proof run_s150_7_down_edge's "
           "own frame 05 makes for the EXACT arm)")

    # ---- drop on an EMPTY PC cell: the loss screen, an item that TRAVELS ---------
    for _ in range(10):
        s.tap("R", settle=150)
    s.shot("04_box11_empty", "S150-8: R x10 -> box 11 ('Qo', 0/30) -- this "
           "cartridge's own completely empty box, cursor on an empty cell with no "
           "further navigation needed")
    s.tap("A", settle=300)
    s.shot("05_loss_screen", "S150-8: A on the empty cell -- 'WHAT WON'T TRANSFER' / "
           "'Item: ESCAPE ROPE travels' (item id 19, item_g2_to_g3 mapped and "
           "allowed by this met_game's cart mask) / 'IVs come from DVs, nature "
           "from EXP' / 'Met: this game, traded' / A = transfer  B = cancel -- no "
           "KEEP AS IS / MAKE LEGAL row: CHIKORITA is a base-form species, never "
           "below pk_evo_floor() at any level, so gb_bank_down_gen3's decision-7 "
           "check (pdna_gen12.c:2765-2780) never fires for any of bank_plant.c's "
           "seven planted cells (five native slots 0-4 + two COPY cells at slots "
           "5/6, BACKLOG #150 S150-12 decision 17) -- none qualifies, per this "
           "lane's own brief",
           # BACKLOG #168a review (claims pass): the same #207 boxoam bracket
           # covers this screen too (it is gb_down_loss_screen, the identical
           # function s150-12's copy-loss frames draw). Not a copy cell here, so
           # PDNA_XFER_COPY_NOBACK_L1 never draws -- the always-drawn middle row
           # is "Met: this game, traded" instead, plus the same fixed top row and
           # bottom-row cancel prompt.
           claim=["IVs come from DVs, nature from EXP", "Met: this game, traded", "B = cancel"])

    # ---- confirm: the ledger write is refused on this vehicle (no SD card) -------
    s.tap("A", settle=300)
    s.shot("06_sidecar_folder_wall", "S150-8: A = transfer -> bdc_convert_gen3_core "
           "succeeds in RAM -> the panel reads 'SIDECAR FOLDER' / 'Nothing "
           "transferred.' / 'Press A' (PDNA_SIDECAR_MKDIR_TITLE / "
           "PDNA_SIDECAR_NOTWRITTEN_L2) -- a LEDGER-WRITE refusal: "
           "xfer_down_write()'s own f_mkdir(/PokeDNA/xfer/) (gb_bank_down_gen3, "
           "source/pdna_gen12.c:2797-2799) runs BEFORE the caller's own PC-box "
           "memcpy (drop_held only places `conv` AFTER bank_down_convert_gen3 "
           "returns BANK_DOWN_CONVERTED, which never happens here) -- so this arm "
           "never even reaches the point of writing the Gen-3 PC box itself; "
           "whether the PC write would succeed on this vehicle is UNTESTED, not "
           "just unproven, and carries to hardware (docs/HW-QUEUE.md)")
    s.tap("A", settle=250)
    s.shot("07_still_holding_after_ledger_refusal", "S150-8: still carrying the "
           "SAME item-holding CHIKORITA cell after the ledger-write refusal -- "
           "the Bank origin was never consumed (BANK_DOWN_REFUSED, not CONVERTED)")

    # ---- SAVE FIRST: only reachable after a FIRST landing actually SUCCEEDS, which
    # never happens on this vehicle (the write above always fails at the SIDECAR
    # FOLDER step, so app_xfer_pending() can never read true) -- proved live, not
    # assumed: a second A on the SAME empty cell reproduces the SAME loss screen,
    # not PDNA_XFER_SAVEFIRST_TITLE. Hardware-only (docs/HW-QUEUE.md).
    s.tap("A", settle=300)
    s.shot("08_savefirst_unreachable_here", "S150-8: a SECOND A on the same empty "
           "cell -- pixel-identical to frame 05's loss screen, not 'SAVE FIRST' "
           "(allow_same: this repeat IS the point -- app_xfer_pending() never "
           "became true on this vehicle, so PDNA_XFER_SAVEFIRST_TITLE is "
           "HARDWARE-ONLY, reachable only once a real card write actually lands)",
           allow_same=True,
           # BACKLOG #168a review (claims pass): pixel-identical to frame 05 (the
           # reviewer's own proof), so the same claim.
           claim=["IVs come from DVs, nature from EXP", "Met: this game, traded", "B = cancel"])
    s.tap("B", settle=200)                            # cancel the loss screen, still carrying

    # ---- the party arm (BACKLOG #174, S150-8c): moved to run_s150_8_party_vsd() ----
    # This function's own frames stop here (still holding, after the PC-arm ledger-write
    # refusal). Frames 09-13 used to show the party's OLD "PC BOX FIRST" refusal
    # (bc_is_native(s_held) always denied the party as a destination) -- BACKLOG #174
    # (S150-8c) deleted that refusal and its old "PC BOX FIRST" strings:
    # the party is now a real native landing. Proving the LANDING needs a real SD
    # write, which THIS vehicle's flash chip cannot serve (frame 06/08 above, the
    # SIDECAR FOLDER wall) -- so the party chain moved to run_s150_8_party_vsd(),
    # a --vsd-attached session (BACKLOG #179's virtual SD, lane s179-a2, merged into
    # the main this lane branched from), driven by --s150-8-party.
    return s


def run_s150_8_party_vsd(core_mod, image_mod, rom_ruby: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #174 (S150-8c): the party arm of the CONVERTING DOWN edge -- a native
    Bank cell converts into a real Gen-3 record and joins the party, FULLY HEALED
    (D7's new loss-screen row), through bank_down_convert_gen3_party() ->
    gb_bank_down_gen3() -> app_party_place_held(). Split out of run_s150_8_gen3_arm()
    (above) because proving a LANDING needs a real SD write, and that function's own
    vehicle (a plain fused Emerald.gba, no --vsd) hits the SIDECAR FOLDER wall at the
    very first ledger write (its own frames 06/08) -- there is no way to reach a REAL
    write on that vehicle. This function instead REQUIRES `--vsd <img>` on the CLI
    (BACKLOG #179's virtual SD, lane s179-a2, merged into the main this lane branched
    from) so `f_mkdir`/`sf_write_verified` succeed for real and `vsd_report()` can
    assert exactly which files changed -- design doc docs/briefs/s150-8cd-design.md
    OQ2's own answer ("the party LANDING is proven by frames AND a vsd_diff").

    `rom_ruby` MUST be `tools/fuse_sav.py <pokedna-delta-artless.gba> Ruby.sav` --
    NOT Emerald.sav. Found live, not assumed: Emerald.sav's own party (this repo's
    corpus, gba-toolkit/roms/Emerald.sav) already has SIX members, so there is no ADD
    slot to land on at all (party_strip_overlay's own `addslot = (s_holding && n < 6)
    ? n : -1` is -1 whenever n >= 6) -- the loss screen becomes UNREACHABLE on that
    save, only the PARTYSWAP refusal is (every row is an occupied swap target, and
    the PARTYFULL3 refusal never fires either, since sel can never equal n_now(6)
    when there is no add slot to select). Ruby.sav's own party has FOUR members
    (verified with a one-off host tool linked against gen3_read_saveblock1() +
    party_count(), not eyeballed), so an ADD slot exists at bank_plant.c's own slot 0
    (a plain CHIKORITA, the SAME cell run_s150_8_gen3_arm's own docstring uses for
    its PC-arm proof).

    Nav recipe, verified live against this exact fused image (probe screenshots
    under /tmp/pdna-s150-8cd-corpus/explore17/ex17_*.png during this lane's own
    development, not guessed):
      Bank entry + pick up slot 0 (plain CHIKORITA, RIGHT x0 -- it is already the
        cursor's start position): the SAME `START,DOWN,A` -> `A,DOWN,A` -> `DOWN x5`
        recipe run_s150_8_gen3_arm() uses for its own item-holding slot 3.
      PARTY: UP once off the PC grid's own top row (one press, PC source, not
        is_bank) -> A opens party_strip_overlay()'s own picker. The cursor SEEDS to
        sel=1 (MUST-FIX 8's own one-time seed, `if (rows >= 2) sel = 1`), on an
        OCCUPIED row -- an immediate A here is the PARTYSWAP refusal (D2(a)),
        needing no navigation. DOWN x3 from the seed (sel 1 -> 4) reaches the ADD
        slot for a 4-member party (addslot == n == 4); A there converts the cell and
        opens the NEW loss screen (D7's `PDNA_XFER_PARTYLAND_L1` row); A again
        confirms the transfer -- THIS is the write `vsd_report()` catches. B from the
        box screen afterward triggers `flush_on_exit()`'s own "Save changes?"
        confirm; A there runs `app_commit_pc()` (a real FLASH1M write, verified by
        the SAVED panel) -> `app_xfer_promote()` -> `pdna_bank_flush_deletions()`.

    BACKLOG #174/#175 review D5 (fix pass): the original claim here -- that
    `/PokeDNA/bank/box00.box` "never appears in the vsd diff" on this vehicle -- was
    FALSE. Frame 10 ("10_saved") is taken INSIDE app_commit_pc()'s own blocking
    "SAVED / Press A" msg_wait, before app_xfer_promote() and
    pdna_bank_flush_deletions() have run at all; the extra A tap right after 10_saved
    dismisses that msg_wait and lets both actually execute, and box00.box (the Bank's
    own defer-delete flush, D3's identity-byte fix's whole reason to exist) DOES
    appear in the vsd diff at that point -- proven directly below, not merely
    structurally. Left un-provable on this vehicle: the PARTYFULL3 (PARTY IS FULL)
    refusal likewise needs the party to reach six members first (two more real
    landings + saves after this one, from bank_plant.c's other two convertible
    cells, slot 1 PIK and slot 3 item-CHIKORITA) -- attempted during this lane's own
    development and found to need more session-state care (the Bank re-plants fresh
    on every re-entry) than this lane's remaining budget could safely verify without
    risking a mis-captioned frame (BACKLOG #184's own lesson); left as an HW-only row
    (XFER-C9r) rather than shipped as an unverified claim."""
    gb_shots.assert_vehicle(rom_ruby, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    print("== BACKLOG #174 (S150-8c): the party arm (--vsd) ==")
    if gb_shots._DEFAULT_VSD_IMG is None:
        raise RuntimeError("--s150-8-party requires --vsd <img.img> on the command "
                            "line (BACKLOG #179's virtual SD) -- there is no way to "
                            "prove a real party landing without it")

    s = gb_shots.Session(core_mod, image_mod, rom_ruby, out_dir, "s150_8_party_")
    s.run(700)
    s.vsd_snapshot()   # reset the diff baseline AFTER boot's own log.txt/MIGRATED writes

    s.tap("START", settle=80)
    s.tap("DOWN", settle=60)
    s.tap("A", settle=150)
    s.shot("00_bank", "S150-8c: the Bank, opened from a Ruby session whose party has "
           "FOUR members (Ruby.sav, this repo's corpus) -- box 0 shows bank_plant.c's "
           "seven cells, cursor starts on slot 0 (plain CHIKORITA)")
    s.tap("A", settle=150)                          # native-cell whitelist menu
    s.press_n("DOWN", 2, settle=60)                        # VIEW/EDIT -> LEGALITY -> MOVE
    s.tap("A", settle=150)                          # pick up -> carrying
    s.press_n("DOWN", 5, settle=150)                # off the Bank grid -> the Ruby PC
    s.shot("01_landed_pc", "S150-8c: carrying the plain CHIKORITA cell, landed on "
           "Ruby's own PC box, still carrying")

    s.tap("UP", settle=150)
    s.shot("02_tab_focus_party", "S150-8c: UP once off the PC grid's top row while "
           "carrying -- tab focus lands on PARTY directly (unchanged from before "
           "this lane: a PC source is not is_bank, one press)")
    s.tap("A", settle=250)
    s.shot("03_party_overlay", "S150-8c: A on the PARTY tab -- party_strip_overlay's "
           "own picker, FOUR party rows + the empty ADD slot + CANCEL; the cursor "
           "SEEDS onto an occupied row (MUST-FIX 8's own `sel = 1` one-time seed), "
           "unchanged from before this lane")

    # ---- D2(a): A on the seeded (occupied) row -- CAN'T SWAP HERE, no navigation ---
    s.tap("A", settle=300)
    s.shot("04_swap_refusal", "S150-8c NEW: A on the seeded occupied row -- "
           "'CAN'T SWAP HERE / A Bank Pokemon can only join / an empty party "
           "slot.' (D2(a)) -- REPLACES the old 'PC BOX FIRST' refusal this lane "
           "deleted; the ledger is untouched (no A=transfer was ever offered)",
           claim=["CAN'T SWAP HERE", "A Bank Pokemon can only join",
                  "an empty party slot."])
    s.tap("A", settle=200)                          # dismiss (msg_wait, A only)
    s.shot("05_still_holding", "S150-8c: dismissed, still carrying the same "
           "CHIKORITA cell -- the refusal consumed nothing (D2's whole point: "
           "every pre-flight runs BEFORE the conversion arm)")

    # ---- the real landing: DOWN x3 from the seed reaches the ADD slot (sel=4) ------
    s.press_n("DOWN", 3, settle=150)
    s.shot("06_on_add", "S150-8c: cursor on the empty ADD slot (seed sel=1 + "
           "DOWNx3 = sel=4, the add slot for this 4-member party)")
    s.tap("A", settle=300)
    s.shot("07_loss_screen", "S150-8c NEW: A on the ADD slot -- the conversion runs "
           "and the shared loss screen draws WITH D7's new row -- 'IVs come from "
           "DVs, nature from EXP' / 'Met: this game, traded' / 'Joins your party, "
           "fully healed.' / A = transfer  B = cancel. No KEEP AS IS / MAKE LEGAL "
           "row: a base-form CHIKORITA is never below pk_evo_floor() at any level",
           claim=["IVs come from DVs, nature from EXP", "Met: this game, traded",
                  "Joins your party, fully healed.", "A = transfer", "B = cancel"])

    # ---- the write itself: A = transfer. RED DEMO first (BACKLOG #184: a proof that
    # cannot fail is not a proof) -- assert vsd_report() actually goes RED (sys.exit(1))
    # on a deliberately wrong expected set, THEN take the real, correct one. ----------
    s.tap("A", settle=400)
    s.shot("08_landed", "S150-8c NEW: A = transfer -> the ledger write lands for "
           "real (this vehicle's --vsd virtual SD, unlike run_s150_8_gen3_arm's own "
           "plain-fused vehicle) -- back at the PC box, no longer carrying "
           "(app_party_place_held returned true)")
    try:
        s.vsd_report(expect_changed=["/PokeDNA/this/path/does/not/exist.pds"])
        raise RuntimeError("RED DEMO FAILED: vsd_report() did not exit(1) on a "
                            "deliberately wrong expect_changed set -- the assertion "
                            "the brief requires is NOT provable; see BACKLOG #184")
    except SystemExit as e:
        if e.code != 1:
            raise RuntimeError(f"RED DEMO: vsd_report() exited {e.code}, not 1")
        print("  [RED DEMO] vsd_report(expect_changed=[wrong path]) correctly "
              "exited 1 -- the proof below is not inert")
    changed = s.vsd_report(expect_changed=[
        "/PokeDNA/xfer/540E42FE7925AA15.pds",   # the ledger entry (xr_key_g3, this
                                                 # cell's own PID+otId -- deterministic
                                                 # for this corpus save, re-derive if
                                                 # the corpus ever changes)
        "/PokeDNA/bank/bank.meta",               # pdna_bank_next_serial()'s own
                                                 # meta_save(), fired by this Bank
                                                 # entry (NOT the deferred-delete
                                                 # flush -- see this function's own
                                                 # docstring, box00.box never appears)
        # /PokeDNA/xfer/MIGRATED dropped here (review D7 fix pass): D7 fixed
        # vsd_snapshot() to flush before copying the image, so the post-boot
        # snapshot right above this call now correctly captures MIGRATED (a
        # boot-time, one-time marker) as part of the BASELINE instead of missing
        # it -- it no longer shows as newly-added by this landing.
        "/PokeDNA/log.txt",                     # the triple logger's own append
    ])
    print(f"  [VSD] landing vsd_diff (GREEN, expected set matched): {sorted(changed)}")
    s.vsd_snapshot()   # reset the baseline again so the save-step diff below is ONLY
                       # what the save itself changes, not a re-report of the landing

    # ---- exit-save: B from the box -> 'Save changes?' -> A -> app_commit_pc() -----
    s.tap("B", settle=300)
    s.shot("09_save_prompt", "S150-8c: B from the box -- flush_on_exit()'s own "
           "'Save changes? / Save the moved Pokemon?' confirm (unchanged by this "
           "lane -- #175's SAVE NOW? is a DIFFERENT prompt, in front of a SECOND "
           "drop, not this exit-time one)",
           claim=["Save changes?", "Save the moved Pokemon?", "A = yes", "B = no"])
    s.tap("A", settle=600)
    s.shot("10_saved", "S150-8c: A = yes -- app_commit_pc() runs a REAL FLASH1M "
           "write (this delta build's own flash chip, verified) -- 'SAVED / Flash "
           "written + verified.' STILL inside app_commit_pc()'s own blocking "
           "msg_wait here (review D5): app_xfer_promote() and "
           "pdna_bank_flush_deletions() have NOT run yet -- they run only after "
           "this msg_wait's own A dismisses it, below.",
           claim=["SAVED", "Flash written + verified."])
    s.tap("A", settle=400)
    s.shot("11_promoted_flushed", "S150-8c review D5: the extra A that dismisses "
           "10_saved's own msg_wait -- THIS is where app_xfer_promote() (cell 1's "
           "ledger entry PENDING -> CLAIMED) and pdna_bank_flush_deletions() (the "
           "Bank's own defer-delete, box00.box) actually run")
    changed2 = s.vsd_report(expect_changed=[
        "/PokeDNA/bank/box00.box",              # pdna_bank_flush_deletions()
                                                 # deletes cell 1's origin Bank
                                                 # slot for real -- D3's identity-
                                                 # byte fix, now OBSERVED, not just
                                                 # structurally proven
        "/PokeDNA/config.cfg",                  # found live: the exit-save path
                                                 # also persists config state here
        "/PokeDNA/log.txt",                     # the triple logger's own append
        "/PokeDNA/xfer/540E42FE7925AA15.pds",   # app_xfer_promote(): PENDING ->
                                                 # CLAIMED (content changed, not
                                                 # added -- the entry already
                                                 # existed from the landing step)
    ])
    print(f"  [VSD] post-save vsd_diff (GREEN, expected set matched): "
          f"{sorted(changed2)} -- the promotion rewrites the ledger file and the "
          f"flush creates box00.box, BOTH observed here (review D5 fix -- the old "
          f"'unobservable via SD' claim was false)")
    return s


def run_s150_8d_savenow(core_mod, image_mod, rom_ruby: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #175 (S150-8d): SAVE NOW? replacing the flat SAVE FIRST refusal when a
    second native->Gen-3 transfer is attempted while one is already PENDING. Needs
    --vsd for the SAME reason run_s150_8_party_vsd() does (a real ledger write must
    actually land for app_xfer_pending() to ever become true) -- sequenced after that
    lane's own OQ2 answer ("after s179-a2"), on the SAME Ruby vehicle.

    `rom_ruby` MUST be `tools/fuse_sav.py <pokedna-delta-artless.gba> Ruby.sav` (the
    SAME fusion run_s150_8_party_vsd() uses). This chain lands into the PC (not the
    party) both times -- #174 and #175 are independent edges; XFER-C24c (the two
    combined) is HW-only, named in this function's own docstring below.

    Nav recipe, verified live (probe screenshots under /tmp/pdna-s150-8cd-corpus/
    explore28..34/ during this lane's own development, not guessed): Ruby's PC box 1
    has only 2 free cells among 28 filled (not enough headroom to navigate cleanly
    twice), so this chain switches to box 9 (`R` x8 from box 1) which the SAME corpus
    save has at 17/30 -- two of its own gaps, row 1 col 1 (DOWN, RIGHT once) and row 1
    col 4 (DOWN, RIGHT x4 -- both confirmed via the left info panel reading
    "(empty)", not eyeballed from the compact 5x6 tag grid, which turned out to be
    genuinely hard to read by eye: several species abbreviate to overlapping 3-letter
    tags). Cell 1 (Bank slot 0, plain CHIKORITA) lands on row1col1 WITHOUT saving.
    Cell 2 (Bank slot 1, plain PIKACHU) is carried to row1col4; dropping it there
    finds app_xfer_pending() already true (cell 1's own entry) and offers SAVE NOW?
    instead of refusing. A = yes runs THREE more automatic/keyed steps in sequence
    (found live, not zero-input as the design's own prose might suggest): a transient
    "Saving -- do not power off" panel (no input, just frames), then A dismisses
    SAVED, then A confirms cell 2's own loss screen -- the whole point is that none
    of these needs new NAVIGATION or a B, just repeated A, "the same gesture"."""
    gb_shots.assert_vehicle(rom_ruby, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    print("== BACKLOG #175 (S150-8d): SAVE NOW? (--vsd) ==")
    if gb_shots._DEFAULT_VSD_IMG is None:
        raise RuntimeError("--s150-8d-savenow requires --vsd <img.img> on the "
                            "command line (BACKLOG #179's virtual SD) -- there is "
                            "no way to reach a pending transfer without it")

    def enter_bank(s: gb_shots.Session) -> None:
        s.tap("START", settle=80)
        s.tap("DOWN", settle=60)
        s.tap("A", settle=150)

    s = gb_shots.Session(core_mod, image_mod, rom_ruby, out_dir, "s150_8d_")
    s.run(700)
    s.vsd_snapshot()

    # ---- cell 1: Bank slot 0 (plain CHIKORITA) -> box9 row1col1. Land, DO NOT SAVE.
    enter_bank(s)
    s.tap("A", settle=150); s.press_n("DOWN", 2, settle=60); s.tap("A", settle=150)   # pick slot0
    s.press_n("DOWN", 5, settle=150)                # off the Bank grid -> Ruby's PC box 1
    s.press_n("R", 8, settle=150)                   # -> box 9 (this corpus's own 17/30)
    s.tap("DOWN", settle=80); s.tap("RIGHT", settle=80)   # row1 col1, an empty cell
    s.shot("00_cell1_on_empty", "S150-8d: carrying the plain CHIKORITA cell, on box "
           "9's row1col1 (empty, confirmed via the info panel)")
    s.tap("A", settle=300)
    s.shot("01_cell1_loss", "S150-8d: the ordinary loss screen (no PARTYLAND row -- "
           "this lands in the PC, not the party)")
    s.tap("A", settle=400)
    s.shot("02_cell1_landed", "S150-8d: A = transfer -- cell 1's ledger entry lands "
           "for real, PENDING, box9 grows to 18/30. Deliberately NOT saved -- this "
           "is the pending transfer #175's own prompt fires on")
    changed1 = s.vsd_report(expect_changed=[
        "/PokeDNA/xfer/540E42FE7925AA15.pds",   # cell 1's own ledger entry (added)
        "/PokeDNA/bank/bank.meta",               # this Bank entry's own meta_save()
        # /PokeDNA/xfer/MIGRATED dropped here (review D7 fix pass), same reason as
        # run_s150_8_party_vsd's own landing check: the flushed post-boot snapshot
        # now correctly carries MIGRATED as part of the baseline.
        "/PokeDNA/log.txt",
    ])
    print(f"  [VSD] cell1 landing vsd_diff (GREEN): {sorted(changed1)}")
    s.vsd_snapshot()

    # ---- cell 2: Bank slot 1 (plain PIKACHU) -> box9 row1col4 -- app_xfer_pending()
    # is already true -> SAVE NOW? instead of the old flat refusal. ----------------
    enter_bank(s)
    s.tap("RIGHT", settle=80)                        # slot0 -> slot1 (plain PIKACHU)
    s.tap("A", settle=150); s.press_n("DOWN", 2, settle=60); s.tap("A", settle=150)
    s.press_n("DOWN", 5, settle=150)
    s.press_n("R", 8, settle=150)                    # box1 -> box9 again (re-navigate)
    s.tap("DOWN", settle=80); s.press_n("RIGHT", 4, settle=80)   # row1 col4, empty
    s.shot("03_cell2_on_empty", "S150-8d: carrying the plain PIKACHU cell, on box "
           "9's row1col4 (the OTHER empty cell)")
    s.tap("A", settle=400)
    s.shot("04_savenow", "S150-8d NEW: A on the ADD target -- 'SAVE NOW? / One "
           "transfer is waiting for the game / save. Save it now?' (wraps after "
           "\"game\", not after \"save.\" -- found live, not assumed) -- REPLACES "
           "the old flat SAVE FIRST refusal (its own strings stay live for the two "
           "restore-side sites, D18, untouched by this lane)",
           claim=["SAVE NOW?", "One transfer is waiting for the game",
                  "save. Save it now?", "A = yes", "B = no"])

    # RED DEMO (BACKLOG #184): the whole A = yes chain below is one continuous
    # gesture the brief calls out by name -- prove vsd_report() actually goes RED on
    # a wrong expectation before trusting the real one.
    s.tap("A", settle=300)
    s.shot("05_saving", "S150-8d: A = yes -- the transient 'Saving -- do not power "
           "off / Writing flash save...' panel (no input; found live that this is "
           "a REAL frame the harness must wait through, not instantaneous)")
    try:
        s.vsd_report(expect_changed=["/PokeDNA/this/path/does/not/exist.pds"])
        raise RuntimeError("RED DEMO FAILED: vsd_report() did not exit(1) on a "
                            "deliberately wrong expect_changed set")
    except SystemExit as e:
        if e.code != 1:
            raise RuntimeError(f"RED DEMO: vsd_report() exited {e.code}, not 1")
        print("  [RED DEMO] vsd_report(expect_changed=[wrong path]) correctly "
              "exited 1 -- the proof below is not inert")

    s.tap("A", settle=400)
    s.shot("06_saved", "S150-8d: app_xfer_save_now() ran app_commit_pc() (a real "
           "FLASH1M write, verified) -> app_xfer_promote() (cell 1's own PENDING "
           "entry flips to CLAIMED) -> pdna_bank_flush_deletions() -- 'SAVED / "
           "Flash written + verified.' A dismisses it",
           claim=["SAVED", "Flash written + verified."])
    s.tap("A", settle=400)
    s.shot("07_loss2", "S150-8d: the SAME gesture continues onto cell 2's own loss "
           "screen, no re-press, no new navigation -- exactly #175's own point",
           claim=["IVs come from DVs, nature from EXP", "A = transfer", "B = cancel"])
    s.tap("A", settle=400)
    s.shot("08_landed2", "S150-8d: A = transfer -- cell 2's ledger entry lands, box9 "
           "grows to 19/30 (both CHI and PIK now visible in the grid)")
    changed2 = s.vsd_report(expect_changed=[
        "/PokeDNA/xfer/540E42FE7925AA15.pds",   # cell 1's entry -- CONTENT changed
                                                 # (PENDING -> CLAIMED, app_xfer_promote)
        "/PokeDNA/xfer/BECFDD2BC68C1D06.pds",   # cell 2's own new entry (added,
                                                 # PENDING -- its OWN save has not
                                                 # run yet)
        "/PokeDNA/bank/box00.box",               # pdna_bank_flush_deletions() found
                                                 # real entries to clear this time
                                                 # (unlike run_s150_8_party_vsd's own
                                                 # single-landing chain) and rewrote
                                                 # the Bank's own box file -- added
        "/PokeDNA/log.txt",
    ])
    print(f"  [VSD] cell2 SAVE NOW + landing vsd_diff (GREEN): {sorted(changed2)}")

    # ---- the decline path: B on the SAME prompt -- a FRESH scratch --vsd image and
    # Session (S150-8d D15's first bullet: "still holding, nothing written, the old
    # START > SAVE route still works"), so cell 2's own landing above (which just
    # dirtied box9/bank.meta/the ledger) cannot contaminate this check. Built via the
    # SAME tools/vsd_img.c mkimg gb_shots.py's own vsd_report() already compiles on
    # demand -- no new build step for the operator, no second --vsd flag needed. */
    scratch_dir = Path(tempfile.mkdtemp(prefix="s150_8d_decline_vsd_"))
    scratch_img = scratch_dir / "decline16.img"
    binp = gb_shots._vsd_img_bin()
    r = subprocess.run([str(binp), "mkimg", str(scratch_img), "16"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"S150-8d decline sub-chain: mkimg failed: {r.stderr.strip()}")
    s2 = gb_shots.Session(core_mod, image_mod, rom_ruby, out_dir, "s150_8d_decline_",
                          vsd_img=scratch_img)
    s2.run(700)
    s2.vsd_snapshot()
    enter_bank(s2)
    s2.tap("A", settle=150); s2.press_n("DOWN", 2, settle=60); s2.tap("A", settle=150)   # slot0
    s2.press_n("DOWN", 5, settle=150)
    s2.press_n("R", 8, settle=150)
    s2.tap("DOWN", settle=80); s2.tap("RIGHT", settle=80)          # row1col1, empty
    s2.tap("A", settle=300)
    s2.tap("A", settle=400)                                        # land cell 1, don't save
    # BACKLOG #179 A3: vsd_snapshot() copies the ON-DISK image as-is; it does NOT
    # flush first (vsd_report() is the one that flushes, per its own docstring) --
    # calling vsd_snapshot() twice in a row with no vsd_report() between them
    # produces two IDENTICAL (both unflushed) snapshots, silently discarding
    # everything cell 1 just wrote. vsd_report() here forces the flush; the
    # vsd_snapshot() right after resets the diff baseline to the JUST-FLUSHED state
    # (mirrors run_s150_8_party_vsd's own working report-then-snapshot pattern,
    # never snapshot-then-snapshot).
    s2.vsd_report()
    s2.vsd_snapshot()
    enter_bank(s2)
    s2.tap("RIGHT", settle=80)                                     # slot1 (PIKACHU)
    s2.tap("A", settle=150); s2.press_n("DOWN", 2, settle=60); s2.tap("A", settle=150)
    s2.press_n("DOWN", 5, settle=150)
    s2.press_n("R", 8, settle=150)
    s2.tap("DOWN", settle=80); s2.press_n("RIGHT", 4, settle=80)   # row1col4, empty
    s2.tap("A", settle=400)
    s2.shot("decline_00_savenow", "S150-8d D15 decline: the SAME SAVE NOW? prompt, "
           "on a fresh scratch --vsd image (cell 1's own landing above must not "
           "contaminate this check)",
           claim=["SAVE NOW?"])
    s2.tap("B", settle=300)
    s2.shot("decline_01_declined", "S150-8d D15: B = no -- BANK_DOWN_REFUSED, still "
           "carrying the PIKACHU cell (footer 'A drop B cancel'), nothing written "
           "for cell 2 -- the old START > SAVE route still works for cell 1's own "
           "still-PENDING entry")
    changed_decline = s2.vsd_report(expect_changed=["/PokeDNA/log.txt"])
    print(f"  [VSD] decline vsd_diff (GREEN, expect ONLY the append-only log): "
          f"{sorted(changed_decline)}")
    s.taken += s2.taken
    s.skipped += s2.skipped
    return s


def run_s150_8_bridge(core_mod, image_mod, rom_gold: Path, rom_red: Path,
                      out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-8: the CONVERTING DOWN edge's GB_BRIDGE arm (a native cell
    crosses Gen 1 <-> Gen 2, source/pdna_gen12.c gb_bank_down_bridge()), post-59dd45c
    (the merged-tree review fix: the bridge now converts to the MOUNTED session's own
    generation, not its own source generation).

    KNOWN DEVIATION FROM THE BRIEF'S OWN WORDING, found live and reported here rather
    than silently substituted: the brief's own text asks to carry the planted Gen-2
    CHIKORITA cell into Red's grid and expects 'the bridge preview ... -> confirm ->
    the delta wall' from THAT gesture. That specific combination is IMPOSSIBLE with
    bank_plant.c's current data, not a harness miscalibration -- xr_time_capsule_
    block() (source/xfer_rec.c:26-43) is the FIRST thing bdc_convert_gb_core() checks,
    unconditionally, before any loss screen exists, and CHIKORITA's dex (152) always
    exceeds gb_max_species(GB_GEN1)=151 for ANY Gen-2->Gen-1 drop, item-holding or
    not, occupied destination or not. So CHIKORITA -> Red can only ever reach the
    species time-capsule refusal (confirmed live below, and already the SAME frame
    run_s150_7_down_edge's own frame 12 shows) -- never the preview. The brief's own
    SEPARATE ask two sentences later ("a planted cell with dex > 151 -> NO GEN 1
    FORM-class message") is exactly what CHIKORITA -> Red demonstrates; the two asks
    cannot both be it. The preview/confirm/wall sequence is demonstrated below on the
    ONE direction bank_plant.c's data can actually reach it: the Gen-1 PIKACHU cell
    (dex 25, always allowed into Gen 2 -- xr_time_capsule_block() returns 0
    unconditionally whenever dst_gen != GB_GEN1) carried into `rom_gold`. This is the
    SAME gb_bank_down_bridge() function and the SAME GB_BRIDGE arm the brief is
    asking about, run in the one direction this planted data can reach end to end.

    GAP CLOSED (BACKLOG #212 review D3(b)): leg (3) below plants a NEW cell
    (source/bank_plant.c's bank_plant_gen2_badmoves_cell(), box 1 slot 7) with a
    species that DOES clear the Gen-1 floor (dex 1, Bulbasaur) and TWO moves out of
    range for Gen 1 -- the frame 09 comment below asked for exactly this ("a plant
    addition ... is needed to shoot it"). #212 also removed xr_time_capsule_block's
    own move-bound refusal entirely (tc==2 can no longer fire, source/pdna_gen12.c's
    own comment on gb_bank_down_bridge) and replaced it with the per-slot clip/fill/
    modal BACKLOG #150 S150-10 gave PASTE (GB) -- so leg (3) reaches the SWAP-ROW
    MODAL, not a time-capsule message, which is the more complete demonstration of
    what #212 actually built.

    `rom_gold`/`rom_red` = tools/fuse_gb.py <pokedna-delta-artless.gba> Gold.gbc
    Gold.sav / Red.gb Red.sav (the SAME two images run_s150_7_down_edge() uses).

    Nav recipe (probe screenshots under /tmp/s150-78-trace/s150-8-bridge/, not
    guessed): Bank entry is run_s150_7_down_edge's own boot_to_grid + UP_INTO_BANK(3)
    + UP_TO_ROW0(4); box 0's cursor starts on slot 0 (plain CHIKORITA) -- ONE RIGHT
    reaches slot 1 (PIKACHU) on the Gold image; no move is needed on the Red image
    (slot 0 IS the CHIKORITA cell already). Pick-up and the DOWN_OFF_BANK=5 hop are
    IDENTICAL to run_s150_7_down_edge's own pick_up_chikorita(). On Gold, R x13
    reaches this cartridge's own box 13 (17/20, room) -- the SAME box run_s150_7_
    down_edge's own EXACT-arm test uses; the GB_BRIDGE arm is not gated by the
    84a43b8 occupied-refusal (that gate is GEN3-only), so the cursor's default
    (occupied) position on this box needs no further navigation, matching
    run_s150_7_down_edge's frame-11/12 finding that a fully-occupied Red box (20/20)
    dispatches fine too."""
    gb_shots.assert_vehicle(rom_gold, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    gb_shots.assert_vehicle(rom_red, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    print("== BACKLOG #150 S150-8: the DOWN edge (GB_BRIDGE arm) ==")
    UP_INTO_BANK = 3
    UP_TO_ROW0 = 4
    DOWN_OFF_BANK = 5

    def boot_to_grid(s: gb_shots.Session) -> None:
        s.run(700)                       # #279: lands on the grid by itself (no info page, no tap)
        s.run(100)

    # ---- (1) Gen 1 -> Gen 2: PIKACHU carried into Gold -- the REACHABLE direction
    # for the preview/confirm/wall sequence (see the docstring's own deviation note) --
    sg = gb_shots.Session(core_mod, image_mod, rom_gold, out_dir, "s150_8b_gold_")
    boot_to_grid(sg)
    sg.press_n("UP", UP_INTO_BANK, settle=100)
    sg.press_n("UP", UP_TO_ROW0, settle=60)
    sg.shot("00_bank", "S150-8 bridge: Gold's own Bank, box 0's seven planted cells "
            "(CHI/PIK/EGG/CHI/DMG at slots 0-4, plus BACKLOG #150 S150-12 decision "
            "17's two COPY cells at slots 5/6), cursor on slot 0")
    sg.tap("RIGHT", settle=100)
    sg.shot("01_cursor_pikachu", "S150-8 bridge: cursor moved RIGHT x1 to slot 1 -- "
            "the Gen-1 PIKACHU cell (dex 25, no species/move ever blocks a "
            "Gen-1->Gen-2 drop: xr_time_capsule_block() returns 0 whenever "
            "dst_gen != GB_GEN1, unconditionally)")
    sg.tap("A", settle=150)                     # native-cell whitelist menu
    sg.press_n("DOWN", 2, settle=60)                   # VIEW/EDIT -> LEGALITY -> MOVE
    sg.tap("A", settle=150)                     # MOVE -> carrying
    sg.press_n("DOWN", DOWN_OFF_BANK, settle=150)
    sg.shot("02_carrying_on_pc", "S150-8 bridge: carrying PIKACHU, back on Gold's "
            "own PC grid -- xg_bank_down_arm resolves GB_BRIDGE (cell_gen 1 != "
            "dst_gen 2)")
    for _ in range(13):
        sg.tap("R", settle=150)
    sg.shot("03_box13_room", "S150-8 bridge: R x13 -> this cartridge's own box 13 "
            "(17/20, room) -- the SAME box run_s150_7_down_edge's EXACT-arm test "
            "uses; cursor sits on box13's own default (occupied) cell, which needs "
            "no further navigation since GB_BRIDGE is not occupied-gated")
    sg.tap("A", settle=300)
    sg.shot("04_bridge_preview", "S150-8 bridge: A -- gb_paste_loss_screen's own "
            "'WHAT WON'T TRANSFER' preview -- 'Nature and ability' / 'Met place / "
            "level / ball' rows shown (loss->nature/ability and loss->met_data/ball "
            "all true for this Gen-1->Gen-2 conversion); NO friendship/pokerus row "
            "(loss->pokerus_dropped and loss->friendship_dropped are both false for "
            "this specific mon -- the brief's own 'friendship row' is conditional, "
            "PDNA_SIDECAR_LOSS_POKERUS, and does not apply to every bridge "
            "transfer, only when gen3_to_gb's own conversion actually drops it); "
            "no dropped-item row (PIKACHU carries no item)",
            # BACKLOG #180 retrofit: the bridge arm must show its OWN honest footer
            # (PDNA_XFER_BRIDGE_STAYS, "the Bank slot is emptied when it lands"), NOT
            # PASTE's stale PDNA_SIDECAR_LOSS_STAYS ("the copy in your Gen-3 save
            # stays") -- there is no Gen-3 copy on a bridge transfer, the Bank slot IS
            # the only copy and it is consumed on landing (59dd45c). claim_absent
            # proves the old wording is gone, not just that the new wording is present.
            claim=["The Bank slot is emptied when it lands."],
            claim_absent=["The copy in your Gen-3 save stays."])
    # BACKLOG #179 lane s179-a4 A5: --vsd changes WHICH refusal this frame shows.
    # Found live (not assumed): xfer_down_write()'s own f_mkdir(/PokeDNA/xfer/) is
    # exactly the flashcartio_*_sector seam --vsd replaces, so with --vsd attached
    # it SUCCEEDS -- gbs_insert() then also succeeds (RAM-only) -- but
    # gb_persist("xferdown") (source/pdna_gen12.c:3955) is refused UNCONDITIONALLY
    # under #ifdef PDNA_DELTA (:4398-4423, "GAME BOY SAVE / Edits are in-session
    # only in the emulator build.") with NO vsd_attached() check at all -- a
    # SEPARATE, deliberate policy wall this lane's brief did not ask to touch and
    # this lane does not touch. So the observable effect of --vsd here is case (c)
    # (SIDECAR FOLDER, xfer_down_write's own mkdir refusal) becoming case (a)
    # (GAME BOY SAVE, gb_persist's refusal) -- the ledger write genuinely lands for
    # real, proven below with a real vsd_report(), not claimed. gb_bank_down_bridge
    # DOES call xfer_down_undo() on the gb_persist failure (source/pdna_gen12.c:
    # 3956-3960, "roll the ledger entry back") -- but the .pds this run actually
    # WRITES still shows up in the final vsd_report() below (found live, not
    # assumed): xfer_down_undo()'s own f_unlink()/sf_write_verified() call is only
    # BEST-EFFORT (its own comment: "a failure here only logs, it never blocks"),
    # so on THIS vehicle the entry survives -- an open question for BACKLOG
    # (does f_unlink genuinely fail against this harness's FAT16 image, or does
    # gb_persist's own PDNA_DELTA-only refusal race xfer_down_undo some other way),
    # not something this lane's narrow brief (wire --vsd, prove it moves the
    # refusal point) resolves. The real, OBSERVED set is asserted below.
    sg.tap("A", settle=300)                     # A = transfer
    if sg.vsd is not None:
        sg.shot("05_gameboy_save_wall", "S150-8 bridge (--vsd): A = transfer -> "
                "xfer_down_write()'s own f_mkdir(/PokeDNA/xfer/) SUCCEEDS for real "
                "(this vehicle's --vsd virtual SD) -> gbs_insert() succeeds -> but "
                "gb_persist(\"xferdown\") is STILL refused ('GAME BOY SAVE / Edits "
                "are in-session only in the emulator build.') -- an UNCONDITIONAL "
                "#ifdef PDNA_DELTA policy (source/pdna_gen12.c:4398-4423) with no "
                "vsd_attached() check, separate from the flashcartio seam this "
                "lane's brief scopes; this is CASE (a), NOT case (c) -- the "
                "refusal point genuinely moved, proving the ledger write itself "
                "landed before gb_persist's own wall undid it")
        try:
            sg.vsd_report(expect_changed=["/PokeDNA/this/path/does/not/exist.pds"])
            raise RuntimeError("RED DEMO FAILED: vsd_report() did not exit(1) on a "
                                "deliberately wrong expect_changed set -- the "
                                "assertion the brief requires is NOT provable")
        except SystemExit as e:
            if e.code != 1:
                raise RuntimeError(f"RED DEMO: vsd_report() exited {e.code}, not 1")
            print("  [RED DEMO] vsd_report(expect_changed=[wrong path]) correctly "
                  "exited 1 -- the proof below is not inert")
        # The REAL diff (found live via the RED demo's own [VSD DIFF FAILED] "got="
        # line, not assumed or hand-derived): the Bank-entry's own bank.meta (this
        # visit's own pdna_bank_next_serial()/meta_save()), the triple logger's
        # log.txt, AND the ledger entry xfer_down_write() wrote -- xfer_down_undo()
        # ran (its own log_line fires either way) but did not make the entry
        # disappear from this vehicle's image, per the comment above.
        changed = sg.vsd_report(expect_changed=[
            "/PokeDNA/bank/bank.meta",
            "/PokeDNA/log.txt",
            "/PokeDNA/xfer/314FD4CF809D0B9D.pds",   # xr_key_g3-equivalent ledger
                                                     # key for this cell -- deterministic
                                                     # for this corpus save, re-derive
                                                     # (run once with a deliberately-
                                                     # wrong set and read the "got="
                                                     # line) if the corpus changes.
        ])
        print(f"  [VSD] gb_persist-wall vsd_diff (GREEN, expected set matched -- "
              f"the .pds SURVIVES despite gb_persist's own refusal, see this "
              f"branch's own comment above): {sorted(changed)}")
        sg.tap("A", settle=250)                     # dismiss
        sg.shot("06_still_holding", "S150-8 bridge (--vsd): still carrying the "
                "same PIKACHU cell after gb_persist's own refusal -- box 13 "
                "unchanged (17/20) even though a ledger entry now exists on the "
                "card for it (BANK_DOWN_REFUSED: the caller never marks the Bank "
                "origin consumed on this path)")
    else:
        sg.shot("05_sidecar_folder_wall", "S150-8 bridge: A = transfer -> the panel "
                "reads 'SIDECAR FOLDER' / 'Nothing transferred.' / 'Press A' "
                "(PDNA_SIDECAR_MKDIR_TITLE/PDNA_SIDECAR_NOTWRITTEN_L2) -- CASE (c), a "
                "LEDGER-WRITE refusal: gb_bank_down_bridge() calls xfer_down_write() "
                "(source/pdna_gen12.c:3096) BEFORE gbs_insert() (:3100), and this "
                "vehicle's f_mkdir(/PokeDNA/xfer/) fails first (no SD card) -- so "
                "gbs_insert()/gb_persist() NEVER RUN on this vehicle. This is NOT frame "
                "04's docstring case (a) ('GAME BOY SAVE / Edits are in-session only', "
                "which would mean gbs_insert succeeded and only the final persist "
                "refused) and NOT case (b) (PDNA_SIDECAR_XFER_REFUSED_TITLE + a GBS "
                "status, which would mean gbs_insert itself refused, the PRE-59dd45c "
                "signature) -- the chain never reaches either of those checks, so "
                "59dd45c's own fix (converting to the mounted session's generation) is "
                "UNTESTED on this vehicle, not disproven: hardware must prove it "
                "(docs/HW-QUEUE.md)")
        sg.tap("A", settle=250)                     # dismiss
        sg.shot("06_still_holding", "S150-8 bridge: still carrying the same PIKACHU "
                "cell after the ledger-write refusal -- box 13 unchanged (17/20)")

    # ---- (2) Gen 2 -> Gen 1: CHIKORITA carried into Red -- the two time-capsule
    # refusals the brief asks for; only the species one is reachable with the
    # current planted data (see the docstring's own deviation note) ----------------
    sr = gb_shots.Session(core_mod, image_mod, rom_red, out_dir, "s150_8b_red_")
    boot_to_grid(sr)
    sr.press_n("UP", UP_INTO_BANK, settle=100)
    sr.press_n("UP", UP_TO_ROW0, settle=60)
    sr.shot("07_red_bank", "S150-8 bridge: Red's own Bank, the SAME generation-"
            "agnostic box 0 plant, cursor on slot 0 (the plain CHIKORITA cell, "
            "dex 152)")
    sr.tap("A", settle=150)
    sr.press_n("DOWN", 2, settle=60)                 # VIEW/EDIT -> LEGALITY -> MOVE (native menu, #271)
    sr.tap("A", settle=150)
    sr.press_n("DOWN", DOWN_OFF_BANK, settle=150)
    sr.shot("08_red_carrying", "S150-8 bridge: carrying CHIKORITA, back on Red's "
            "own (Gen-1) grid")
    sr.tap("A", settle=250)
    sr.shot("09_no_gen1_form", "S150-8 bridge: A -- 'NO GEN 1 FORM' / 'CHIKORITA: "
            "no Gen 1 form.' / 'Press A' (PDNA_XFER_TC_TITLE/PDNA_XFER_TC_SPECIES_"
            "FMT) -- xr_time_capsule_block()'s species-floor check (tc==1: dex 152 "
            "> gb_max_species(GB_GEN1)=151) fires before ANY loss screen; this is "
            "the ONE time-capsule refusal bank_plant.c's data can reach -- every "
            "planted cell uses move id 33 (Tackle, plant_gen2_chikorita() and the "
            "Pikachu block both, source/bank_plant.c:25/51), a Gen-1-legal move id "
            "well under gb_max_move(GB_GEN1)=165, so NO planted cell can ever "
            "trigger the move-based refusal (tc==2, '<MOVE>: not in Gen 1.') -- "
            "none is planted for that case, exactly as this lane's own brief's "
            "fallback anticipates; a plant addition (a Gen-2-only move on a "
            "species that clears the dex<=151 floor) is needed to shoot it")

    # ---- (3) BACKLOG #212 review D3(b): the plant addition frame 09's own comment
    # called for -- a Gen-2 BULBASAUR (dex 1, clears the Gen-1 species floor) with
    # TWO moves (200, 230) out of range for Gen 1, planted by bank_plant_box_full()
    # at box 1 slot 7 (source/bank_plant.c's own bank_plant_gen2_badmoves_cell()).
    # Carried into Red, this reaches gb_bank_down_bridge's per-slot move clip/fill
    # and the swap-row modal instead of the species-floor refusal frame 09 hit --
    # the SAME per-slot rule BACKLOG #150 S150-10 gave PASTE (GB), applied to the
    # bridge by #212, and the missing half of this chain's own brief.
    #
    # Nav, re-derived directly against this vehicle with a FRESH session (not a
    # continuation of `sr` above -- frame 09's message, once dismissed, returns to
    # "still carrying CHIKORITA" (matching leg (1)'s own 05->06 transition), not to
    # the Bank; a first cut of this leg continued on `sr` straight after 09 and
    # silently spent its own R/DOWN/RIGHT/A presses steering the STILL-CARRIED
    # CHIKORITA around Red's real corpus boxes instead of ever picking up the new
    # plant -- caught by this leg's own claim= checks failing, not by eye): boot
    # fresh -> UP_INTO_BANK -> UP_TO_ROW0 (box 0, same as frame 07) -> R x1 (BANK 1
    # -> BANK 2, bank_plant_box_full's 30-cell box) -> DOWN x1, RIGHT x1 -> box 1
    # slot 7 (BULBASAUR/BADMOVE). CRITICAL, found live: this screen's grid is
    # COLS=6 (source/pdna_box.c) with RIGHT WRAPPING WITHIN THE ROW ("cur - COLS +
    # 1" at the last column, never advancing to the next row) -- seven flat RIGHT
    # presses from slot 0 lands on slot 1 (box1's own copy of box0's Gen-1 PIKACHU,
    # byte-identical OT/level to bank_plant_box0()'s slot 1), not slot 7; confirmed
    # both ways with a direct probe (the GB1/GB2 summary badge, the species name,
    # and the move list all disagreed with the wrong navigation, agreed with DOWN
    # x1 + RIGHT x1). Pick-up (A -> DOWN -> A -> DOWN_OFF_BANK) is IDENTICAL to legs
    # (1)/(2) above. Red's own BOX1 (index 0, the DOWN_OFF_BANK landing box) is
    # 20/20 full (tests/host_gbsurgery_tool.c --list, matching frame 02's own "GB
    # BOX1 20/20" caption) -- gb_accept_down_hook's EXACT-arm capacity gate would
    # refuse a same-gen drop there, but this is GB_BRIDGE (cross-gen), which does
    # not share that gate; the LATE capacity check gb_bank_down_bridge itself runs
    # (source/pdna_gen12.c ~3881, AFTER the modal) is against a genuinely-empty box
    # instead: R x5 -> box 5 (16/20 on Guy's own Red.sav, tests/host_gbsurgery_tool.
    # c --list), room to spare.
    s3 = gb_shots.Session(core_mod, image_mod, rom_red, out_dir, "s150_8b_red3_")
    boot_to_grid(s3)
    s3.press_n("UP", UP_INTO_BANK, settle=100)
    s3.press_n("UP", UP_TO_ROW0, settle=60)
    s3.tap("R", settle=150)                       # BANK 1 -> BANK 2 (bank_plant_box_full)
    s3.shot("10_bank2_slot0", "S150-8 bridge D3(b): Red's Bank, box 1 (BANK 2, "
            "30/30) -- bank_plant_box_full()'s own 30-cell fixture, cursor on "
            "slot 0 (CHIKORITA, same as box 0's own slot 0)")
    s3.tap("DOWN", settle=150)
    s3.tap("RIGHT", settle=150)
    s3.shot("11_cursor_badmoves", "S150-8 bridge D3(b): cursor moved DOWN x1, "
            "RIGHT x1 (COLS=6 -- see this leg's own nav note) to slot 7 -- the "
            "planted BULBASAUR (dex 1, OT BADMOVE/9999), moves OUTRAGE (200) and "
            "SWEET SCENT (230), both > gb_max_move(GB_GEN1)=165 but <= "
            "gb_max_move(GB_GEN2)=251 (legal on its own generation)")
    s3.tap("A", settle=150)                        # native-cell whitelist menu
    s3.press_n("DOWN", 2, settle=60)                       # VIEW/EDIT -> LEGALITY -> MOVE
    s3.tap("A", settle=150)                         # MOVE -> carrying
    s3.press_n("DOWN", DOWN_OFF_BANK, settle=150)
    s3.shot("12_carrying_badmoves", "S150-8 bridge D3(b): carrying the BULBASAUR "
            "badmoves cell, back on Red's own (Gen-1) grid -- BOX1, 20/20 (no room "
            "here; see the R x5 below)")
    s3.press_n("R", 5, settle=150)
    s3.shot("13_box5_room", "S150-8 bridge D3(b): R x5 -> box 5 (16/20 on Guy's "
            "own Red.sav, tests/host_gbsurgery_tool.c --list -- room for the late "
            "capacity check gb_bank_down_bridge runs after the modal)")
    s3.tap("A", settle=300)                         # bdc_convert_gb_core runs -> loss screen
    s3.shot("14_loss_screen", "S150-8 bridge D3(b): A -- gb_paste_loss_screen's "
            "'WHAT WON'T TRANSFER' preview (nature/ability, met data/ball, "
            "pokerus/friendship rows for this Gen-2->Gen-1 conversion) -- reached "
            "because dex 1 clears the species-floor check frame 09's CHIKORITA "
            "(dex 152) never got past")
    s3.tap("A", settle=gb_shots.BIG_SETTLE)          # proceed -> s_busy_reading() draws (D5's own (aj) pin), THEN the cold scan starts
    # BACKLOG #212 re-verify R1: the artifact that used to sit over "your" and fail
    # an exact substring check was NOT unrelated -- it was the #207 class of bug on
    # THIS arm: gb_bank_down_bridge() had no boxoam_suspend()/boxoam_resume() around
    # its dialogs, so the carry cursor (art_fallbacks.c hand OAM) and the glove's
    # era badge (box_oam.c boxoam_carry_badge, purple '2') were drawn over the busy
    # screen. Now bracketed (source/pdna_gen12.c gb_bank_down_bridge, ~:3844-3857),
    # so the claim is restored.
    s3.shot("15_busy_reading", "BACKLOG #210/#212 review D5, R1: the busy screen "
            "covers the bridge's OWN gb_paste_fill_moves() cold "
            "gb_create_locate_rom() scan (source/pdna_gen12.c's (aj) check) -- "
            "captured mid-scan, before the swap modal below. Both lines ('Reading "
            "your ROM...' / 'This can take a moment.') are legible, no cursor/badge "
            "pixels over the text now that gb_bank_down_bridge brackets this dialog "
            "with boxoam_suspend()/boxoam_resume()",
            claim=["Reading your ROM...", "This can take a moment."])
    s3.run(4000 - gb_shots.BIG_SETTLE)               # let the cold scan finish (measured pixel-identical well before 4,000 total)
    s3.shot("16_swap_modal_two_rows", "S150-8 bridge D3(b), R1: the swap-row modal "
            "(gb_paste_legal_screen_ex) -- TWO swap rows, one per bad slot "
            "('OUTRAGE -> <a Gold/Red level-up move>', 'SWEET SCENT -> <a Gold/Red "
            "level-up move>'); 'KEEP AS IS: not possible here' in dim text (nbad > "
            "0 greys this row, decision 8); 'SELECT = MAKE LEGAL (swap moves)'; "
            "'Either way it comes back unchanged.'; 'B = cancel' -- the per-slot "
            "rule BACKLOG #150 S150-10 gave PASTE (GB), applied to the bridge by "
            "#212, now proven live with TWO bad slots (not one), the frame this "
            "chain's own brief originally asked for and the docstring above found "
            "unreachable with the OLD plant data. No cursor/badge pixels over "
            "'Either way it comes back unchanged.' (the row the badge used to sit "
            "on) now that this dialog is bracketed too (~:3862-3865)",
            claim=["OUTRAGE", "SWEET SCENT", "KEEP AS IS: not possible here",
                   "SELECT = MAKE LEGAL (swap moves)", "B = cancel",
                   "Either way it comes back unchanged."])

    sg.taken += sr.taken + s3.taken
    sg.skipped += sr.skipped + s3.skipped
    return sg


def _extract_gb_rom_offline(fused_image_path: Path) -> Path:
    """BACKLOG #214b: the offline twin of gb_shots.Session._gb_rom_file() -- same
    directory-parse, same "exactly one embedded ROM" contract -- but standalone
    (no live Session/core, --selftest-captions never boots mGBA), called against
    a STORED `claim_gb_image` path (the fused .gba the ORIGINAL capture used,
    manifest.json's own record of it) rather than a Session's in-memory
    self.rom_path. A plain passthrough of that fused path to check_gb() is WRONG
    (BACKLOG #214 review item 5): the C driver's rom_gbui_open() expects a raw
    .gb/.gbc, and handing it a multi-megabyte fused .gba fails with a locate
    error even on a genuinely correct shot -- this function does the exact same
    extraction the live capture did, so the offline re-check reads the identical
    bytes gbscr_text()/rom_gbui_glyph() actually rendered from."""
    blob = fused_image_path.read_bytes()
    rec_off = fuse_gb.locate_record_permissive(blob, str(fused_image_path))
    dir_off, dir_size = fuse_gb.read_record(blob, rec_off)
    if not dir_size:
        raise ValueError(f"claim_gb offline re-check: {fused_image_path} has no fused "
                          "GB directory at all")
    entries = fuse_gb.parse_directory(blob, dir_off, dir_size)
    rom_entries = [e for e in entries if e["type"] in (fuse_gb.TYPE_ROM_GEN1, fuse_gb.TYPE_ROM_GEN2)]
    if len(rom_entries) != 1:
        raise ValueError(f"claim_gb offline re-check: {fused_image_path} carries "
                          f"{len(rom_entries)} embedded GB ROM(s), expected exactly 1")
    e = rom_entries[0]
    rom_bytes = blob[e["offset"]:e["offset"] + e["size"]]
    tmp_dir = Path(tempfile.mkdtemp(prefix="dgb_shots_claimgb_offline_"))
    ext = ".gbc" if e["type"] == fuse_gb.TYPE_ROM_GEN2 else ".gb"
    tmp_path = tmp_dir / f"embedded{ext}"
    tmp_path.write_bytes(rom_bytes)
    return tmp_path


def main(argv=None) -> int:
    """BACKLOG #179 A3 review D1 (BLOCKER): _main_dispatch() below constructs every
    Session this process will build; whichever of its many CLI branches ran (or a
    --selftest-captions sys.exit(1) fired instead of returning at all), every
    attached --vsd write must reach disk before the process exits. finally still runs
    when SystemExit propagates through it, so this covers every exit path without
    touching any of _main_dispatch()'s own dozens of `return 0`/`return 1` lines
    (deliberately: other lanes add new CLI branches to that function constantly, and
    a touched `return` in each one would be a guaranteed merge conflict with every
    one of them -- the same reasoning _write_manifest()'s own sys.exit(1) docstring
    already gives for not editing every dispatch branch)."""
    try:
        return _main_dispatch(argv)
    finally:
        gb_shots.flush_live_vsd_sessions()


def _main_dispatch(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", type=Path,
                     help="pokedna-delta-gb.gba -- the ONE combined image (boot picker, "
                          "standalone GB mount, AND the nested NV_GB import all live here "
                          "since BACKLOG #68a retired the separate delta-gb-only image)")
    ap.add_argument("--shell-only", action="store_true",
                     help="U2a: only run_gbscreen_shell() against --image, skip the "
                          "boot-picker/standalone/NV_GB flows")
    ap.add_argument("--u2c-trainer", action="store_true",
                     help="U2c: only run_u2c_trainer() against --image (Red's own "
                          "trainer card), skip the other flows")
    ap.add_argument("--u3-trainer", choices=("gold", "crystal"),
                     help="U3: only run_u3_trainer() against --image for the named "
                          "game (Gold's or Crystal's own trainer card), skip the "
                          "other flows")
    ap.add_argument("--u4-bag", choices=("red", "yellow"),
                     help="U4: only run_u4_bag() against --image for the named game "
                          "(Red's or Yellow's own Item bag) -- --image MUST be a "
                          "ONE-Gen-1-ROM fused image matching this choice (BACKLOG "
                          "#98's fused-image-by-generation-only harness gap)")
    ap.add_argument("--u4-empty", action="store_true",
                     help="N6(e): only run_u4_empty() against --image -- --image "
                          "MUST be fused with a Red save whose Items pocket was "
                          "zeroed by docs/shots/rvu4/mkbag.c (count 0)")
    ap.add_argument("--u5-pack", choices=("gold", "crystal"),
                     help="U5: only run_u5_pack() against --image for the named "
                          "game (Gold's or Crystal's own Pack + PC store) -- "
                          "--image MUST be a ONE-Gen-2-ROM fused image matching "
                          "this choice (BACKLOG #98's fused-image-by-generation-"
                          "only harness gap, same as --u4-bag)")
    ap.add_argument("--d7-gold", action="store_true",
                     help="N6(f): only run_d7_gold() against --image -- --image "
                          "MUST be a Gold-only fused image (Gold.gbc+Gold.sav)")
    ap.add_argument("--b86-clock", choices=("crystal", "red"),
                     help="BACKLOG #86/#108: only run_b86_clock()/"
                          "run_b86_clock_gen1_fallback() against --image for the "
                          "named game -- --image MUST be a ONE-ROM fused image "
                          "matching this choice (Crystal.gbc+Crystal.sav, or "
                          "Red.gb+Red.sav for the Gen-1 fallback shot), same "
                          "single-ROM posture as --d7-gold")
    ap.add_argument("--b87-dex", choices=("red", "crystal"),
                     help="BACKLOG #87: only run_b87_dex() against --image for the "
                          "named game -- --image MUST be a ONE-ROM fused image "
                          "matching this choice (Red.gb, or Crystal.gbc -- either "
                          "way, the .sav must have had `--op dexset 100 0` applied "
                          "first -- see run_b87_dex()'s own docstring for why)")
    ap.add_argument("--b124-dexicons", choices=("red", "crystal"),
                     help="BACKLOG #124: only run_b124_dexicons() against --image for "
                          "the named game -- --image MUST be a ONE-ROM fused image "
                          "matching this choice. Pair with --b124-fallback to expect "
                          "a .sav-only image (no .gbc/.gb ROM) and the icon-store "
                          "fallback shot instead of GB ROM icons.")
    ap.add_argument("--b124-fallback", action="store_true",
                     help="BACKLOG #124: with --b124-dexicons, expect --image to carry "
                          "no GB ROM (fuse_gb.py with just the .sav) -- the no-GB-ROM "
                          "fallback shot.")
    ap.add_argument("--b124-bobcheck", choices=("red", "crystal"),
                     help="BACKLOG #124 review A5: run_b124_bobcheck() against "
                          "--image -- two frames 30 apart on the dex grid, diffed, "
                          "to prove the caught-cell bob animation runs (or does not) "
                          "on the named session.")
    ap.add_argument("--b124-sdcount", action="store_true",
                     help="BACKLOG #124 step 2: run measure_dex_sd_reads() against "
                          "--image (a Crystal single-ROM fused image, GB ROM present) "
                          "and print every captured 'perf ' log line instead of taking "
                          "screenshots.")
    ap.add_argument("--b196-sdreads", choices=("red", "crystal"),
                     help="BACKLOG #196: measure_dex_sd_reads() against --image (a "
                          "ONE-ROM fused image of the NAMED game, GB ROM present) -- "
                          "the generalised (Red or Crystal) form of --b124-sdcount, "
                          "for the before/after 'perf dex: sd Nr' comparison this "
                          "backlog's report needs on BOTH generations, not just "
                          "Crystal. See measure_dex_sd_reads()'s own docstring for "
                          "why this counter is architecturally 0 on this (PDNA_DELTA) "
                          "vehicle regardless of what this feature does -- hardware-"
                          "only.")
    ap.add_argument("--b208-dexcache", choices=("red", "crystal"),
                     help="BACKLOG #208 step 3: run_b208_dexcache() against --image "
                          "(a ONE-ROM fused image of the NAMED game, GB ROM present) "
                          "-- captures every 'dexart: page N fetch=X hit=Y' log line "
                          "across cold entry / same-page repaint / a no-scroll "
                          "cursor move / a row scroll (each line prints one phase "
                          "LATE -- see that function's own docstring), plus three "
                          "repaint frames for a byte-compare against main.")
    ap.add_argument("--b85-daycare", choices=("red", "gold"),
                     help="BACKLOG #85: only run_b85_daycare() against --image for "
                          "the named game (Red's one-slot Day Care, or Gold's "
                          "two-slot Day Care + compatibility) -- --image MUST be a "
                          "ONE-ROM fused image matching this choice (same "
                          "single-ROM harness gap as --u4-bag/--u5-pack)")
    ap.add_argument("--b88-flags", choices=("red", "crystal"),
                     help="BACKLOG #88: only run_b88_flags() against --image for the "
                          "named game (pdna_gbflags.c's own Flags & counters screen) "
                          "-- --image MUST be a ONE-ROM fused image matching this "
                          "choice, same single-ROM posture as --b86-clock")
    ap.add_argument("--b88-flags-d6", action="store_true",
                     help="BACKLOG #88 D6 review: only run_b88_flags_d6() against "
                          "--image -- the WARN confirm (both directions) and the "
                          "READONLY 'STORY FLAG' message, neither reached by "
                          "--b88-flags crystal's own flow -- --image MUST be a "
                          "Crystal-only fused image (Crystal.gbc+Crystal.sav)")
    ap.add_argument("--b114-yard", choices=("red", "gold"),
                     help="BACKLOG #114: only run_b114_yard() against --image for the "
                          "named game (the Day-Care YARD scene, not the D1/D6 "
                          "mechanics --b85-daycare already covers) -- --image MUST be "
                          "a ONE-ROM fused image matching this choice (same "
                          "single-ROM harness gap as --b85-daycare)")
    ap.add_argument("--r1-xfer", action="store_true",
                     help="BACKLOG #104 R1: only run_r1_xfer() against --image -- "
                          "--image MUST be pokedna-delta-artless.gba fused with an "
                          "ordinary Gen-3 .sav, an 80-byte clip record for an "
                          "underlevelled evolved species (fuse_sav.py --clip), and "
                          "Gold.gbc+Gold.sav (fuse_gb.py, ONE Game Boy ROM -- GOLD, "
                          "not Red: see run_r1_xfer()'s own docstring for why)")
    ap.add_argument("--r1-xfer-red", action="store_true",
                     help="BACKLOG #112: only run_r1_xfer_red() against --image -- "
                          "--image MUST be pokedna-delta-artless.gba fused the SAME "
                          "way as --r1-xfer but with Red.gb+Red.sav (fuse_gb.py) "
                          "instead of Gold -- proves the PDNA_DELTA-only fused-ROM "
                          "fallback (source/pdna_gen12.c) reaches the R1 screen on "
                          "Red instead of 'NO GEN-1 ROM'")
    ap.add_argument("--s150-10", action="store_true",
                     help="BACKLOG #150 S150-10 (G-H8): only run_s150_10() against "
                          "--image -- --image MUST be pokedna-delta-artless.gba fused "
                          "the SAME way as --r1-xfer (Gold.gbc+Gold.sav) except the "
                          "--clip record is re-moved to SURF/BITE/ROCK TOMB/PROTECT "
                          "(extract_gen3_record.c's --moves flag) -- ONE bad slot on "
                          "Gen 2, the swap-row modal's mixed case")
    ap.add_argument("--s150-10-two-bad", action="store_true",
                     help="BACKLOG #150 S150-10: only run_s150_10_two_bad() against "
                          "--image -- a SEPARATE fused image, --clip re-moved to "
                          "ROCK TOMB/OVERHEAT/0/0 -- TWO bad slots on Gen 2")
    ap.add_argument("--s150-10-four-bad", action="store_true",
                     help="BACKLOG #150 S150-10 decision 8.7: only "
                          "run_s150_10_four_bad() against --image -- a SEPARATE "
                          "fused image, --clip re-moved to ROCK TOMB/AERIAL ACE/"
                          "BULK UP/DIVE (all four > 251) -- exploratory, see its "
                          "own docstring for why the zero-move refusal is not "
                          "guaranteed with a ROM present")
    ap.add_argument("--m1-map", action="store_true",
                     help="M1 (BACKLOG #91): only run_m1_map() against --image -- "
                          "--image MUST be a Red-only fused image (Red.gb+Red.sav)")
    ap.add_argument("--m3-map-teleport", action="store_true",
                     help="M3 (BACKLOG #91): only run_m3_map_teleport() against "
                          "--image -- --image MUST be a Red-only fused image "
                          "(Red.gb+Red.sav), same as --m1-map")
    ap.add_argument("--m1-map-vclamp", action="store_true",
                     help="M1 (BACKLOG #91) vertical-clamp demo: only "
                          "run_m1_map_vclamp() against --image -- --image MUST be a "
                          "Red-only fused image built from a save WARPED onto Route 17 "
                          "(tests/host_gbsurgery_tool.c's --op warp 28 8 68, see that "
                          "function's own docstring for why)")
    ap.add_argument("--m1-map-g2", action="store_true",
                     help="M1-G2 (BACKLOG #91): only run_m1_map_gen2() against --image "
                          "-- --image MUST be a Crystal-only fused image (Crystal.gbc+"
                          "Crystal.sav, no Emerald.sav)")
    ap.add_argument("--m1-map-g2-wrong-game", action="store_true",
                     help="M1-G2 (BACKLOG #91) wrong-game refusal: only "
                          "run_m1_map_gen2_wrong_game() against --image -- --image MUST "
                          "carry Crystal.gbc paired to a GOLD save (Gold.sav, no "
                          "Gold.gbc)")
    ap.add_argument("--m1-map-g2-no-rom", action="store_true",
                     help="M1-G2 (BACKLOG #91) no-ROM refusal: only "
                          "run_m1_map_gen2_no_rom() against --image -- --image MUST "
                          "carry a Gen-2 save (Gold.sav) with NO Gen-2 ROM fused")
    ap.add_argument("--gbmon", action="store_true",
                     help="BACKLOG #92: only run_gbmon() against --image -- the new "
                          "ITEM row on the Gen-2 mon menu. --image MUST be a "
                          "Gen-2-only fused image (Gold.gbc+Gold.sav or "
                          "Crystal.gbc+Crystal.sav, tools/fuse_gb.py, one ROM per "
                          "image -- BACKLOG #98)")
    ap.add_argument("--b93-menu", choices=("red", "gold"),
                     help="BACKLOG #93: only run_b93_menu() against --image -- "
                          "DUPLICATE/TO DAY-CARE/EXPORT .pk on the mon menu plus "
                          "the GB box menu's EXPORT ALL/RELEASE ALL. --image MUST "
                          "be a ONE-ROM fused image matching the choice (Gold.gbc+"
                          "Gold.sav or Red.gb+Red.sav, tools/fuse_gb.py, no "
                          "Emerald.sav -- BACKLOG #98)")
    ap.add_argument("--b154-boxmenu", choices=("red", "yellow", "gold"),
                     help="BACKLOG #154: only run_b154_boxmenu() against --image -- "
                          "the box-options menu's Rename row, hidden on a source "
                          "with no box-name table (Gen 1) rather than shown then "
                          "refused. --image MUST be a ONE-ROM fused image matching "
                          "the choice (Gold.gbc+Gold.sav or Red.gb+Red.sav / "
                          "Yellow.gb+Yellow.sav, tools/fuse_gb.py, no Emerald.sav "
                          "-- BACKLOG #98)")
    ap.add_argument("--b187-chains", action="store_true",
                     help="BACKLOG #187/#193/#191a/#192, review fix 2: runs "
                          "run_b187_chain_a/b/c() against --image in sequence -- "
                          "Chain A (SELECT toggles NORMAL<->MOVE on an empty "
                          "cell), Chain B (DUPLICATE on a full box opens F3's "
                          "picker and lands), Chain C (CREATE in a box with "
                          "room). --image MUST be the flight-shaped image "
                          "(Emerald.sav + a GB corpus, tools/fuse_gb.py) fused "
                          "with GUY'S OWN Yellow.sav copied to /tmp first (the "
                          "corpus at gba-toolkit/roms/gb/Yellow.sav is read-"
                          "only). Chain D (drag-and-drop) is not here -- see "
                          "this file's own comment right after "
                          "run_b187_chain_c() for why it is impossible on any "
                          "emulator vehicle.")
    ap.add_argument("--b89-hof", choices=("red", "crystal"),
                     help="BACKLOG #89: only run_b89_hof() against --image for the "
                          "named game (Red's or Crystal's own Hall of Fame screen) "
                          "-- --image MUST be a ONE-ROM fused image matching this "
                          "choice, same BACKLOG #98 harness-gap reasoning as "
                          "--u4-bag/--u5-pack/--b90-fly")
    ap.add_argument("--b89-hof-extra", choices=("red-nick", "crystal-nick", "crystal-shiny"),
                     help="BACKLOG #89 D6/NICK: only run_b89_hof_detail_only() -- "
                          "--image MUST be a ONE-ROM fused image whose .sav was "
                          "edited first via tests/host_gbsurgery_tool.c's --op "
                          "hofnick/--op hofdv (a nicknamed mon for *-nick, a shiny "
                          "DV quad + nickname for crystal-shiny; BACKLOG #198 item "
                          "7 replaced the old undocumented byte poke with this "
                          "reproducible recipe) -- see run_b89_hof_detail_only()'s "
                          "own docstring for the exact commands")
    ap.add_argument("--b194-hof", choices=("red", "crystal"),
                     help="BACKLOG #202 F4: only run_b194_hof() against --image -- "
                          "the cold card open, in-frame menu, an EDIT-a-level round "
                          "trip, ADD TEAM, and DELETE TEAM, each through to the "
                          "emulator's own in-session refusal and the shell's "
                          "reopen -- --image MUST be a ONE-ROM fused image matching "
                          "this choice, same posture as --b89-hof")
    ap.add_argument("--b194-hof-no-rom", choices=("red", "crystal"),
                     help="BACKLOG #202 F1: only run_b194_hof_no_rom() against "
                          "--image -- --image MUST be a fused image carrying the "
                          "named game's HoF save with NO Gen ROM fused at all (the "
                          "honest plain-fallback shot)")
    ap.add_argument("--s2-bank", choices=("gold", "red"),
                     help="#120: only run_s2_bank() against --image for the "
                          "named game -- the Bank, reachable from a Game Boy session "
                          "(bank_edge's UP hop). --image MUST be a ONE-ROM fused image "
                          "matching this choice (tools/fuse_gb.py, Gold.gbc+Gold.sav or "
                          "Red.gb+Red.sav, NO Emerald.sav -- the boot must take the "
                          "fused-GB branch, gb_delta_pick_save() skips its own picker "
                          "when n == 1). --s2-bank-clip is a SEPARATE, single-slot "
                          "image (tools/fuse_sav.py --gb ... --clip REC80.BIN, no ROM) "
                          "for the one shot (c) that needs a seeded Gen-3 clipboard "
                          "entry to prove PASTE HERE's absence means something, rather "
                          "than 'nothing was ever copied' -- fuse_gb.py's own directory "
                          "format and fuse_sav.py's single-slot locator are two "
                          "different fusion schemes and cannot be layered onto one "
                          "image without the single-slot fork pre-empting the "
                          "directory one (pdna_main.c checks fused_sav_present() "
                          "before the gb_delta_pick_save() loop).")
    ap.add_argument("--s2-bank-clip", type=Path,
                     help="#120: the single-slot, clip-seeded image for "
                          "--s2-bank's shot (c) -- see --s2-bank's own help.")
    ap.add_argument("--s2-bank-control", action="store_true",
                     help="#120 F1: only run_s2_bank_control() against --image -- "
                          "--image MUST be a plain tools/fuse_sav.py fusion of an "
                          "Emerald.sav onto pokedna-delta-artless.gba (no --gb, no "
                          "--clip) proving CREATE + PASTE HERE survive an ordinary "
                          "Gen-3 session's own Bank visit, unaffected by the F1 gate.")
    ap.add_argument("--s2-control", action="store_true",
                     help="#143: only run_s2_control() against --image -- the Gen-3 "
                          "CONTROL for the Bank menu (what a Gen-3 Bank cell offers: "
                          "TO GAME / PASTE HERE / CREATE). --image MUST be a plain "
                          "tools/fuse_sav.py fusion of an Emerald.sav onto pokedna-"
                          "delta-artless.gba (no --gb, no --clip -- same vehicle as "
                          "--s2-bank-control).")
    ap.add_argument("--s150-2", action="store_true",
                     help="BACKLOG #150 S150-2: only run_s150_2_bank_native() against "
                          "--image -- --image MUST be a plain tools/fuse_sav.py fusion "
                          "of an Emerald.sav onto pokedna-delta-artless.gba (no --gb, "
                          "no --clip -- same vehicle as --s2-bank-control). No fused "
                          "payload needed: the native cells come from source/"
                          "bank_plant.c's PDNA_DELTA-only box_load() hook.")
    ap.add_argument("--s150-3", action="store_true",
                     help="BACKLOG #150 S150-3: only run_s150_3_escape_gate() against "
                          "--image -- --image MUST be a plain tools/fuse_sav.py fusion "
                          "of an Emerald.sav onto pokedna-delta-artless.gba (no --gb, "
                          "no --clip -- same vehicle as --s150-2). The mon-menu "
                          "whitelist (VIEW/MOVE/RELEASE/CANCEL), the deny toast when a "
                          "native cell in hand is dropped into the PC, and VIEW opening "
                          "the real Gen-1/2 summary.")
    ap.add_argument("--s150-13", action="store_true",
                     help="BACKLOG #150 S150-13 (#164): only run_s150_13_carry_badge() "
                          "against --image -- --image MUST be a plain tools/fuse_sav.py "
                          "fusion of an Emerald.sav onto pokedna-delta-artless.gba (no "
                          "--gb, no --clip -- same vehicle as --s150-2/--s150-3). The "
                          "glove's era badge on a carried native Bank cell, both gens "
                          "(slot 0 CHIKORITA/2, slot 1 PIKACHU/1).")
    ap.add_argument("--s150-14", action="store_true",
                     help="BACKLOG #150 S150-14: only run_s150_14_native_edit() against "
                          "--image -- --image MUST be a plain tools/fuse_sav.py fusion "
                          "of an Emerald.sav onto pokedna-delta-artless.gba (no --gb, "
                          "no --clip -- same vehicle as --s150-2/--s150-3). The native "
                          "mon menu's VIEW / EDIT row, entering edit mode in the REAL "
                          "Gen-1/2 summary over a native cell, a DV change, the write-"
                          "confirm panel, and the after-commit grid (a delta image has "
                          "no PDNA_DELTA branch in box_save(), so this proves decision "
                          "7's rollback, not a real write -- see the run function's own "
                          "docstring).")
    ap.add_argument("--s150-15", action="store_true",
                     help="BACKLOG #150 S150-15: only run_s150_15_view_original() "
                          "against --image -- --image MUST be a plain tools/fuse_sav.py "
                          "fusion of an Emerald.sav onto a PDNA_DELTA-built "
                          "pokedna-delta-artless.gba (decision 13's seam compiled in; "
                          "no --gb, no --clip -- same vehicle as --s150-2/--s150-8). "
                          "The GB ORIGINAL row on a converted Gen-3 mon's menu, opening "
                          "the real Gen-1/2 summary read-only over the ledger's "
                          "original80, both on a PC cell and a Bank cell that is "
                          "abroad -- see run_s150_15_view_original()'s own docstring "
                          "for the full nav recipe.")
    ap.add_argument("--s150-11", action="store_true",
                     help="BACKLOG #150 S150-11: only run_s150_11_reconcile() against "
                          "--image -- --image MUST be `make delta-gb`'s own combined "
                          "image (Emerald.sav + Red/Gold/Crystal, same vehicle as "
                          "--s150-12), since the chain needs a Game Boy session (Gold) "
                          "as well as the Gen-3 side. The 21-row START menu with the "
                          "new Transfers row, the empty-state TRANSFERS screen, the "
                          "row dimmed inside a Game Boy session with the honest "
                          "refusal message, and a silent Bank open (no ledger on this "
                          "vehicle -- decision 16, see the run function's own "
                          "docstring for why only the empty state is producible here).")
    ap.add_argument("--b166", action="store_true",
                     help="BACKLOG #166: only run_b166() against --image -- "
                          "--image MUST be `make delta-gb`'s own combined image "
                          "(Emerald.sav + Red/Gold/Crystal). A normal cell's "
                          "occupied-cell menu, per tap -- see run_b166()'s own "
                          "docstring for why the refused-lift case is not shown live.")
    ap.add_argument("--b182", action="store_true",
                     help="BACKLOG #182 (b219): only run_b182_release_gate() against "
                          "--image -- --image MUST be a STANDALONE tools/fuse_gb.py "
                          "image, ONE payload (Red.gb + a /tmp copy of Red.sav whose "
                          "party was trimmed to 1 mon via tests/host_gbsurgery_tool.c "
                          "--op delete party 5/4/3/2/1, fused onto pokedna-delta-"
                          "artless.gba -- same vehicle shape as --b190). Proves the "
                          "1-mon-party PARTY_FLOOR case #166's own corpus could never "
                          "reach: the party cell menu omits BOTH MOVE TO BOX (#166) "
                          "and RELEASE (#182), no shown-then-refused row.")
    ap.add_argument("--b190", action="store_true",
                     help="BACKLOG #190: only run_b190_move_refusal() against --image -- "
                          "--image MUST be a STANDALONE tools/fuse_gb.py image, ONE "
                          "payload (Yellow.gb + Yellow.sav, no Gen-3 save fused): the "
                          "move-picker DUP refusal on the GB editor's MOVES card, "
                          "reproducing (or, after the fix, not reproducing) the ghosted "
                          "refusal-panel-over-redrawn-card glitch -- see the run "
                          "function's own docstring for the full recipe and root cause.")
    ap.add_argument("--b188", action="store_true",
                     help="BACKLOG #188: only run_b188_resume_cell() against --image -- "
                          "--image MUST be the SAME STANDALONE tools/fuse_gb.py image as "
                          "--b190 (Yellow.gb + Yellow.sav, no Gen-3 save fused): pdna_box()'s "
                          "resume-cell hint, both the same-invocation control and the "
                          "outer-re-entry fix (START -> nav menu -> B) -- see the run "
                          "function's own docstring for the full recipe.")
    ap.add_argument("--s150-4", action="store_true",
                     help="BACKLOG #150 S150-4/5 follow-up (lane s150-4-5b, BACKLOG "
                          "#171/#171b), CORRECTED by b199 review D3: only "
                          "run_s150_4_uplift() against --image -- --image MUST be "
                          "`make delta-gb`'s own combined image (Emerald.sav + "
                          "Red/Gold/Crystal). The grab step end to end: CM_MOVE grab "
                          "just HOLDS now (footer 'A drop  B cancel'), no origin "
                          "picker, no refusal -- BACKLOG #199 chain D moved "
                          "BoxXferOps.lift_up (the picker, the pack, "
                          "pdna_bank_next_serial()) off the grab and onto the Bank "
                          "drop (run_s150_9_site2 covers that half). A second A on "
                          "the same cell is a same-cell no-op (drop_held's own "
                          "guard), not a refusal -- there was never anything to "
                          "refuse from this cell. #171b's `s.xfer = &k_gb_xfer` "
                          "wiring is still what makes the picker/refusal reachable "
                          "at all, just one drop later than this help text used to "
                          "say -- see the run function's own docstring for the full "
                          "explanation. The Gen-1 last-party-mon refusal is also NOT "
                          "reachable on this corpus (Red.sav's party is a full "
                          "6/6).")
    ap.add_argument("--s150-7", action="store_true",
                     help="BACKLOG #150 S150-7: only run_s150_7_down_edge() -- the DOWN "
                          "edge (a native cell back into a same-generation Game Boy "
                          "save). Needs TWO images: --image MUST be tools/fuse_gb.py "
                          "<pokedna-delta-artless.gba> Gold.gbc Gold.sav (Gen 2), and "
                          "--s150-7-red MUST be tools/fuse_gb.py <pokedna-delta-"
                          "artless.gba> Red.gb Red.sav (Gen 1) -- see the run function's "
                          "own docstring for why a GB-session fusion (not fuse_sav.py) "
                          "is required this time.")
    ap.add_argument("--s150-7-red", type=Path,
                     help="#150 S150-7: the Gen-1 (Red) fused image --s150-7 also needs "
                          "-- see --s150-7's own help.")
    ap.add_argument("--b142", action="store_true",
                     help="BACKLOG #142: only run_b142_tab_focus_arrival() -- the "
                          "is_bank grid's directional-arrival tab focus. Needs TWO "
                          "images, both `tools/fuse_gb.py <pokedna-delta-artless.gba> "
                          "Gold.gbc Gold.sav`: --image is built from THIS lane's fixed "
                          "source, --b142-before from a scratch pre-fix build of the "
                          "commit this lane branched from (same before/after idiom as "
                          "the two rebuilt images, just source-level instead of a "
                          "second ROM/save).")
    ap.add_argument("--b142-before", type=Path,
                     help="#142: the pre-fix fused image --b142 also needs -- see "
                          "--b142's own help.")
    ap.add_argument("--s150-8", action="store_true",
                     help="BACKLOG #150 S150-8: only run_s150_8_gen3_arm() -- the "
                          "CONVERTING DOWN edge's GEN3 arm (a native cell converts into "
                          "a real Gen-3 record and lands in the current session's own PC "
                          "box). --image MUST be tools/fuse_sav.py <pokedna-delta-"
                          "artless.gba> Emerald.sav (a plain Gen-3 fusion, no --gb -- "
                          "same vehicle shape as --s2-bank-control).")
    ap.add_argument("--s150-9", action="store_true",
                     help="BACKLOG #150 S150-9: only run_s150_9_merge_screen() -- the "
                          "shared per-field MERGE screen (both real-rows and the "
                          "'nothing changed' skip) plus the RESTORED/PENDING refusals, "
                          "all reached via decision 12's planted-ledger read shim. "
                          "--image MUST be tools/fuse_sav.py <pokedna-delta-"
                          "artless.gba> Emerald.sav (a plain Gen-3 fusion, no --gb -- "
                          "same vehicle shape as --s150-8).")
    ap.add_argument("--s150-9-site2", action="store_true",
                     help="BACKLOG #209: only run_s150_9_site2() -- gb_lift_restore, "
                          "site 2 of the restore (a GB-grid lift of a mon whose "
                          "ledger entry has a NATIVE home), reached via the NEW "
                          "PDNA_DELTA shim on xr_path_for_key plus a mount-time seed "
                          "keyed to whatever Red.sav's own box 0 slot 0 mon actually "
                          "is. --image MUST be `make delta-gb`'s own combined image "
                          "(Emerald.sav + Red/Gold/Crystal) -- the SAME vehicle "
                          "--s150-4 uses; the boot-picker path taken to reach Red.sav "
                          "is the one that carries bank_plant_site2_seed(), not the "
                          "blank-flash fork (this image's flash is never blank).")
    ap.add_argument("--s150-8-party", action="store_true",
                     help="BACKLOG #174 (S150-8c): only run_s150_8_party_vsd() -- "
                          "the party arm of the CONVERTING DOWN edge (a native Bank "
                          "cell converts and joins the party, fully healed). "
                          "REQUIRES --vsd <img.img> (BACKLOG #179's virtual SD) -- "
                          "there is no way to reach a real ledger write without it. "
                          "--image MUST be tools/fuse_sav.py <pokedna-delta-"
                          "artless.gba> Ruby.sav (NOT Emerald.sav -- Emerald's own "
                          "corpus party already has six members, so there is no ADD "
                          "slot to land on; Ruby's has four).")
    ap.add_argument("--s150-8d-savenow", action="store_true",
                     help="BACKLOG #175 (S150-8d): only run_s150_8d_savenow() -- "
                          "SAVE NOW? replacing the flat SAVE FIRST refusal for a "
                          "second pending native->Gen-3 transfer. REQUIRES --vsd "
                          "<img.img> (same reason as --s150-8-party). --image MUST "
                          "be tools/fuse_sav.py <pokedna-delta-artless.gba> Ruby.sav "
                          "(the SAME fusion --s150-8-party uses).")
    ap.add_argument("--s150-8-bridge", action="store_true",
                     help="BACKLOG #150 S150-8: only run_s150_8_bridge() -- the "
                          "CONVERTING DOWN edge's GB_BRIDGE arm (Gen 1 <-> Gen 2). "
                          "Needs TWO images: --image MUST be tools/fuse_gb.py "
                          "<pokedna-delta-artless.gba> Gold.gbc Gold.sav (Gen 2), and "
                          "--s150-7-red MUST be tools/fuse_gb.py <pokedna-delta-"
                          "artless.gba> Red.gb Red.sav (Gen 1) -- the SAME two images "
                          "--s150-7 uses (reuses --s150-7-red rather than adding a "
                          "third flag name for the identical Red image).")
    ap.add_argument("--s150-12", action="store_true",
                     help="BACKLOG #150 S150-12: only run_s150_12_copy_edge() against "
                          "--image -- --image MUST be `make delta-gb`'s own combined "
                          "image (Emerald.sav + Red/Gold/Crystal). The read-only "
                          "START > GB import mount's COPY lift (SELECT enters MOVE, "
                          "the origin prompt draws, the lift is then refused cleanly "
                          "at the same no-SD-card wall run_s150_4_uplift() already "
                          "documents for the MOVE lift), then bank_plant.c's two "
                          "PDNA_DELTA-only planted COPY cells (box 0 slots 5/6) prove "
                          "decision 9's DOWN-skips-the-ledger claim live: both land in "
                          "the Emerald PC with the honest NOBACK loss-screen rows and "
                          "no SAVE FIRST wall between them, and the Bank slot reads "
                          "blank afterwards -- see the run function's own docstring "
                          "for the full per-tap trace.")
    ap.add_argument("--b54-romhack", choices=("hack", "control"),
                     help="BACKLOG #54: only run_b54_romhack() against --image for the "
                          "named case -- the ROM-hack banner + the mon-menu refusal it "
                          "wires up. --image MUST be tools/fuse_sav.py <pokedna-delta-"
                          "artless.gba> roms/Emerald.sav (a plain Gen-3 fusion, no --gb, "
                          "same vehicle as --s2-bank-control) fed either 'hack' (a COPY "
                          "of Emerald.gba, made in /tmp only, with 0xA0..0xAB overwritten "
                          "'POKEMON HACK' before fusing -- retail code+version+size, "
                          "wrong title, so rom_identify() classifies HACK(EMERALD)) or "
                          "'control' (the unmodified Emerald.gba, classifies RETAIL).")
    ap.add_argument("--d1-boxname-gate", choices=("hack", "gen1"),
                     help="b199 review D1 fix proof: only run_d1_boxname_gate() against "
                          "--image for the named case -- the #244 banner dialog no "
                          "longer fires on a can_edit()-false refusal. 'hack' needs a "
                          "fusion of pokedna-delta-artless.gba with a hack-flagged "
                          "Emerald.gba (same 0xA0..0xAB recipe as --b54-romhack's own "
                          "'hack' case) + Emerald.sav (fuse_rom.py then fuse_sav.py); "
                          "'gen1' needs a ONE-ROM fused image (Red.gb+Red.sav, no "
                          "Emerald.sav, same BACKLOG #98 reasoning as --gbnames).")
    ap.add_argument("--gbnames", choices=("red", "crystal"),
                     help="gbnames brief: only run_gbnames() against --image for the "
                          "named game -- real Gen-1/Gen-2 item names now shown by "
                          "pdna_gbbag.c/pdna_gbpack_body.inc (source/gb_item_names.c, "
                          "an embedded identifier table). --image MUST be a ONE-ROM "
                          "fused image matching this choice (Red.gb+Red.sav or "
                          "Crystal.gbc+Crystal.sav, same BACKLOG #98 harness-gap "
                          "reasoning as --u4-bag/--u5-pack)")
    ap.add_argument("--b90-fly", choices=("red", "crystal"),
                     help="BACKLOG #90: only run_b90_fly() against --image for the "
                          "named game (Red's or Crystal's own Fly-destination "
                          "screen) -- --image MUST be a ONE-ROM fused image "
                          "matching this choice (same BACKLOG #98 harness-gap "
                          "reasoning as --u4-bag/--u5-pack)")
    ap.add_argument("--b94-boxname", choices=("gold", "crystal"),
                     help="F1 step 3 (BACKLOG #94): only run_b90_boxname() against "
                          "--image for the named game (the Gen-2 box-rename banner) "
                          "-- --image MUST be a ONE-ROM fused image matching this "
                          "choice (Gold.gbc+Gold.sav or Crystal.gbc+Crystal.sav), "
                          "same BACKLOG #98 harness-gap reasoning as --b90-fly")
    ap.add_argument("--b132-portrait", choices=("gold", "red"),
                     help="BACKLOG #132: run_b132_portrait() against --image for the "
                          "named game's own summary portrait, through the boot-picker "
                          "mount AND the nested START > NV_GB entry (RETIRED by "
                          "BACKLOG #239/#277 -- that leg now pins the BANK ONLY "
                          "refusal) -- --image MUST be a COMBINED image (Emerald.sav + "
                          "Red/Gold/Crystal, `make delta-gb`'s own recipe) so both "
                          "mounts are reachable from the one image.")
    ap.add_argument("--b64-import", choices=("gold", "red"),
                     help="BACKLOG #64 (review Fix 3): run_b64_import() against --image "
                          "for the named game -- --image MUST be a COMBINED image "
                          "(Emerald.sav + Red/Gold/Crystal, `make delta-gb`'s own recipe, "
                          "same as run_nav_gb()). Drives the SAME START > nav menu > GB "
                          "import (NV_GB) entry run_nav_gb() already uses -- RETIRED by "
                          "BACKLOG #239/#277: that leg now pins the BANK ONLY refusal -- "
                          "and captures Trainer/Bag-or-Pack/Flags/Dex/Map from the boot "
                          "picker's own direct GB row (the pre-existing resident-image "
                          "mount). The still-HW-only half is "
                          "pdna_gen12_show() itself (the real FIL/SD file-browser entry) "
                          "-- see run_b64_import()'s own docstring.")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "gb")
    ap.add_argument("--cold-start-compare", nargs=2, type=Path, metavar=("LOC_IMAGE", "NOLOC_IMAGE"),
                     help="BACKLOG #68b: measure+report the box-grid cold-start frame cost "
                          "on LOC_IMAGE (fused with the default rom_gb*_open_loc() records) "
                          "vs NOLOC_IMAGE (fused with `fuse_gb.py --no-loc`), diff their final "
                          "frames, and write both as shots into --out. Skips the normal "
                          "--image shot run entirely.")
    ap.add_argument("--b185-cold-locate", type=Path, metavar="NOLOC_IMAGE",
                     help="BACKLOG #185 Step 1: measure locate()'s emulator-floor cold-scan "
                          "cost (Red.sav Gen 1 + Crystal.sav Gen 2) on NOLOC_IMAGE (fused "
                          "with `fuse_gb.py --no-loc`, same convention as --cold-start-compare) "
                          "-- run this against a --before and an --after build to get the "
                          "brief's own comparison. Skips the normal --image shot run entirely.")
    ap.add_argument("--b222", nargs=3, type=Path,
                     metavar=("NICK_IMAGE", "HOFNICK_NOROM_IMAGE", "ITEM_NOROM_IMAGE"),
                     help="BACKLOG #222 review R3: the three missing fixed-font-collapse "
                          "site frames (R1's ui_ascii_next_fixed()). Each image is a "
                          "separately-fused `make delta-artless` base -- NICK_IMAGE = Red.gb "
                          "+ an edited Red.sav (box slot 0 nicknamed PIKA♂ via "
                          "host_gbsurgery_tool's --op nick 0 0), HOFNICK_NOROM_IMAGE and "
                          "ITEM_NOROM_IMAGE = the edited Red.sav ALONE, no ROM fused at all "
                          "(hofnick: team 0 mon 0 renamed PIKA♂ via --op hofnick 0 0; item: "
                          "id 4 qty 10 inserted via --op item items 4 10). Runs "
                          "run_b222_summary_nick()/run_b222_hof_ot()/run_b222_bag_item() in "
                          "that order against their own image. Skips the normal --image shot "
                          "run entirely.")
    ap.add_argument("--b216b", type=Path, metavar="CAFE_IMAGE",
                     help="BACKLOG #216b: the Gen-1/2 summary's Nickname row on a Gold "
                          "box mon renamed CAFé -- the SAME site --b222's NICK_IMAGE "
                          "shoots. CAFE_IMAGE is a separately-fused `make delta-artless` "
                          "base -- Gold.gbc + an edited Gold.sav (box slot 0 renamed via "
                          "host_gbsurgery_tool's --op nick 0 0 \"CAFé\"). Runs "
                          "run_b216b_summary_cafe() against it. Skips the normal --image "
                          "shot run entirely.")
    ap.add_argument("--b200", action="store_true",
                     help="BACKLOG #200: runs run_b200_chain() against --image -- the "
                          "Gen-1/2 grid's phantom cells (blocked-cell paint, cursor "
                          "skip/clamp, the A refusal), four sub-chains: box 12 (0/20) + "
                          "the DOWN-off-blocked-row edge, the GB PARTY pseudo-box "
                          "(capacity 6), box 1 (20/20 full, same blocked rows), and "
                          "Emerald's own PC box (unaffected). --image MUST be the same "
                          "flight-shaped image --b187-chains uses (Emerald.sav + a "
                          "fused Yellow.gb/Yellow.sav, GUY'S OWN Yellow.sav copied to "
                          "/tmp first).")
    ap.add_argument("--b199-chain-d", action="store_true",
                     help="BACKLOG #199 (lane b199): runs run_b199_chain_d() against "
                          "--image -- the drag-and-drop gesture BACKLOG #187's review "
                          "fix 2 could never shoot before this lane. D1: a within-save "
                          "GB move (box1 slot0 -> box12) that asks nothing and writes no "
                          "serial. D2: a GB -> Bank drop (box1 slot0 -> the Bank) that "
                          "does both. Both sub-chains attach their OWN --vsd scratch "
                          "image automatically (mkimg'd into --out) -- do not pass "
                          "--vsd yourself, it would be overwritten. --image MUST be the "
                          "same flight-shaped image --b187-chains/--b200 use (Emerald.sav "
                          "+ a fused Yellow.gb/Yellow.sav, GUY'S OWN Yellow.sav copied "
                          "to /tmp first).")
    ap.add_argument("--selftest-captions", action="store_true",
                     help="BACKLOG #198's original floor (a caption is non-empty and its "
                          "frame file exists) EXTENDED for BACKLOG #184: read --out's "
                          "manifest.json, check every shot's caption is non-empty and its "
                          "frame file exists, AND for every entry carrying \"claim\"/"
                          "\"claim_absent\" (gb_shots.Session.shot()'s own claim= kwarg, "
                          "tools/gb_claims.py) RE-RUN the pixel search against that PNG "
                          "right now, offline (no mGBA/--image needed) -- this is an "
                          "independent re-derivation, not a re-print of whatever "
                          "claim_failed the capture itself already wrote, so a hand-edited "
                          "manifest or a stale PNG is caught too. Exits 1 and prints every "
                          "failure if any caption is empty/whitespace-only, any frame file "
                          "is missing, or any claim/claim_absent fails on re-check; exits 0 "
                          "(and prints the shot + claim-checked counts) otherwise.")
    ap.add_argument("--vsd", type=Path,
                     help="BACKLOG #179 Phase A step A3: attach the harness-hosted "
                          "virtual SD (tools/vsd.py) to every Session this run "
                          "constructs, serving disk_read/disk_write out of this "
                          "tools/vsd_img.c-built .img file. Absent (the default), "
                          "every Session behaves byte-identically to before this "
                          "lane -- see A4's parity gate.")
    # BACKLOG #179 A3 review D6: units are SECTORS for fail_at/lie_after/fail_read_at
    # (a multi-sector FatFs call can cross the threshold mid-call, exactly like
    # tests/hostfat/ramdisk.c's own rd_fail_at/rd_lie_after/rd_fail_read_at) and CALLS
    # for fail_write_in/fail_reads_after (ramdisk.c decrements those by 1 per call,
    # never by the call's sector count) -- see tools/vsd.py's own module docstring for
    # the full per-knob rationale this help text summarizes.
    ap.add_argument("--vsd-protect", action="store_true",
                     help="S4.7 failure injection: every VSD write fails, "
                          "unconditionally, forever (a write-protected volume).")
    ap.add_argument("--vsd-fail-all-writes", action="store_true",
                     help="S4.7: every VSD write fails, unconditionally, forever "
                          "(an EverDrive, by design).")
    ap.add_argument("--vsd-fail-write-in", type=int, default=None,
                     help="S4.7: the NEXT N served VSD write CALLS all fail, then "
                          "heal (not just the Nth -- a run of N).")
    ap.add_argument("--vsd-fail-at", type=int, default=None,
                     help="S4.7: after N successful VSD write SECTORS, the write "
                          "call that crosses that threshold fails once, then heals.")
    ap.add_argument("--vsd-lie-after", type=int, default=None,
                     help="S4.7: after N successful VSD write SECTORS, every write "
                          "reports OK and discards, forever (the card that ACKs and "
                          "keeps nothing).")
    ap.add_argument("--vsd-lie-writes", action="store_true",
                     help="S4.7: every VSD write reports OK and discards, from the "
                          "very first call (no countdown -- the card that was "
                          "already bad).")
    ap.add_argument("--vsd-fail-read-at", type=int, default=None,
                     help="S4.7: after N successful VSD read SECTORS, the read call "
                          "that crosses that threshold fails once, then heals.")
    ap.add_argument("--vsd-fail-reads-after", type=int, default=None,
                     help="S4.7: after N successful VSD read CALLS, every read "
                          "fails, forever (never heals).")
    a = ap.parse_args(argv)
    a.out.mkdir(parents=True, exist_ok=True)

    # BACKLOG #179 Phase A step A3: wire --vsd (and any failure-injection knobs) into
    # every Session this process constructs, BEFORE any run_*() dispatch below --
    # gb_shots.set_default_vsd(None) with no knobs is a true no-op (Session.vsd stays
    # None), so a run without --vsd is unaffected either way.
    gb_shots.set_default_vsd(
        a.vsd,
        protect=a.vsd_protect if a.vsd_protect else None,
        fail_all_writes=a.vsd_fail_all_writes if a.vsd_fail_all_writes else None,
        fail_write_in=a.vsd_fail_write_in,
        fail_at=a.vsd_fail_at,
        lie_after=a.vsd_lie_after,
        lie_writes=a.vsd_lie_writes if a.vsd_lie_writes else None,
        fail_read_at=a.vsd_fail_read_at,
        fail_reads_after=a.vsd_fail_reads_after,
    )

    if a.selftest_captions:
        manifest_path = a.out / "manifest.json"
        if not manifest_path.is_file():
            sys.exit(f"--selftest-captions: {manifest_path}: not a file "
                      "(run this tool with --out pointed at a directory that "
                      "already has a manifest.json)")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        failures: list[str] = []
        claims_checked = 0
        gb_claims_checked = 0
        for entry in manifest.get("shots", []):
            name = entry.get("file", "<no file key>")
            caption = entry.get("caption", "")
            if not caption or not caption.strip():
                failures.append(f"{name}: empty/whitespace-only caption")
            frame_path = a.out / name
            if not frame_path.is_file():
                failures.append(f"{name}: frame file missing ({frame_path})")
                continue  # nothing to re-check a claim against
            claim = entry.get("claim")
            claim_absent = entry.get("claim_absent")
            if claim is not None or claim_absent is not None:
                claims_checked += 1
                for f in gb_claims.check(frame_path, claim=claim, claim_absent=claim_absent):
                    failures.append(f"{name}: {f}")

            # BACKLOG #214b: claim_gb= cannot go through gb_claims.check() above --
            # it needs the RAW embedded .gb/.gbc, not the fused .gba path check()
            # would otherwise be handed (rom_gbui_open() fails to locate anything
            # useful in a multi-megabyte fused image; a naive passthrough produced
            # a false claim failure on a correct shot, review item 5). Re-derive
            # the embedded ROM from claim_gb_image (Session.shot()'s own record of
            # the fused image THIS capture used) via the same directory-parse
            # _gb_rom_file() uses, into a throwaway temp file, then delete it
            # whether the check passed or not -- this loop can run over hundreds
            # of shots and must not leave one temp ROM per entry behind.
            claim_gb = entry.get("claim_gb")
            if claim_gb is not None:
                claim_gb_image = entry.get("claim_gb_image")
                if not claim_gb_image:
                    failures.append(f"{name}: claim_gb present but no claim_gb_image "
                                     "recorded in the manifest -- cannot re-derive the ROM")
                else:
                    gb_claims_checked += 1
                    image_path = Path(claim_gb_image)
                    if not image_path.is_file():
                        failures.append(f"{name}: claim_gb_image {image_path} not a file "
                                         "(offline re-check needs the SAME fused image the "
                                         "live capture used, still present on disk)")
                    else:
                        extracted_rom = None
                        try:
                            extracted_rom = _extract_gb_rom_offline(image_path)
                            for f in gb_claims.check_gb(frame_path, extracted_rom, claim_gb=claim_gb):
                                failures.append(f"{name}: {f}")
                        except ValueError as exc:
                            failures.append(f"{name}: claim_gb offline re-check setup failed: {exc}")
                        finally:
                            if extracted_rom is not None:
                                shutil.rmtree(extracted_rom.parent, ignore_errors=True)
        if failures:
            print(f"--selftest-captions: {len(failures)} failure(s):", file=sys.stderr)
            for f in failures:
                print(f"  {f}", file=sys.stderr)
            return 1
        print(f"--selftest-captions: ok -- {len(manifest.get('shots', []))} shot(s), "
              f"every caption non-empty, every frame file present, "
              f"{claims_checked} shot(s)' claim(s) re-verified against their own PNG, "
              f"{gb_claims_checked} shot(s)' claim_gb(s) re-verified via a freshly "
              f"re-extracted embedded ROM (BACKLOG #214b)")
        return 0

    core_mod, image_mod = gb_shots.load_mgba()

    # the dispatch chain below: every new flag's block sets `ran = True` and
    # ends with `return 0` -- a `ran = False` latch before this chain (lane
    # tiny2, BACKLOG #117) plus `if ran: return 0` right after it is what
    # stops an un-returned flag falling through into the combined boot-picker
    # flow at the bottom of this function.
    if a.cold_start_compare:
        loc_image, noloc_image = a.cold_start_compare
        if not loc_image.is_file():
            sys.exit(f"--cold-start-compare: {loc_image}: not a file")
        if not noloc_image.is_file():
            sys.exit(f"--cold-start-compare: {noloc_image}: not a file")
        ok, skipped = run_cold_start_compare(core_mod, image_mod, loc_image, noloc_image, a.out)
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        return 0

    if a.b185_cold_locate:
        noloc_image = a.b185_cold_locate
        if not noloc_image.is_file():
            sys.exit(f"--b185-cold-locate: {noloc_image}: not a file")
        ok, skipped = run_b185_cold_locate(core_mod, image_mod, noloc_image, a.out)
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        return 0

    if a.b222:
        nick_image, hofnick_norom_image, item_norom_image = a.b222
        for label, p in (("NICK_IMAGE", nick_image),
                          ("HOFNICK_NOROM_IMAGE", hofnick_norom_image),
                          ("ITEM_NOROM_IMAGE", item_norom_image)):
            if not p.is_file():
                sys.exit(f"--b222: {label} {p}: not a file")
        ok, skipped = [], []
        try:
            sess = run_b222_summary_nick(core_mod, image_mod, nick_image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b222 summary nick: {e}")
        try:
            sess = run_b222_hof_ot(core_mod, image_mod, hofnick_norom_image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b222 hof ot: {e}")
        try:
            sess = run_b222_bag_item(core_mod, image_mod, item_norom_image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b222 bag item: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.b216b:
        if not a.b216b.is_file():
            sys.exit(f"--b216b: {a.b216b}: not a file")
        ok, skipped = [], []
        try:
            sess = run_b216b_summary_cafe(core_mod, image_mod, a.b216b, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b216b summary cafe: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if not a.image:
        ap.error("--image is required unless --cold-start-compare is given")
    if not a.image.is_file():
        sys.exit(f"--image: {a.image}: not a file")

    ok, skipped = [], []
    ran = False  # latch: every early-exit flag block sets ran = True; latch returns for all
    if a.shell_only:
        try:
            sess = run_gbscreen_shell(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] gbscreen shell: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.u2c_trainer:
        try:
            sess = run_u2c_trainer(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] u2c trainer: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.u3_trainer:
        try:
            sess = run_u3_trainer(core_mod, image_mod, a.image, a.out, a.u3_trainer)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] u3 trainer ({a.u3_trainer}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.u4_bag:
        try:
            sess = run_u4_bag(core_mod, image_mod, a.image, a.out, a.u4_bag)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] u4 bag ({a.u4_bag}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.u5_pack:
        try:
            sess = run_u5_pack(core_mod, image_mod, a.image, a.out, a.u5_pack)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] u5 pack ({a.u5_pack}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.b89_hof:
        try:
            sess = run_b89_hof(core_mod, image_mod, a.image, a.out, a.b89_hof)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b89 hof ({a.b89_hof}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.b89_hof_extra:
        which, kind = a.b89_hof_extra.rsplit("-", 1)
        if kind == "nick":
            name, cap = "08_nick", (
                f"BACKLOG #89: (D6/NICK, R4) {which}'s team detail with mon 0 "
                "nicknamed via tests/host_gbsurgery_tool.c's --op hofnick "
                "(gb_name_encode) -- proves "
                "hof_detail_render draws GbHofMon.nick, measured against "
                "sys8Font's 8px cells so it NEVER overflows the 240px screen or "
                "the OT-id column. A 10-char species + a full 10-char nickname + "
                "a non-shiny suffix fits WHOLE (29-10-1-7=11 >= 10, see crystal's "
                "own TYPHLOSION \"FLAMETHROW\" shot); the shiny-worst-case honest "
                "trim (9 of 10 chars) is proven on b89_crystal_09_shiny instead")
        else:
            name, cap = "09_shiny", (
                "BACKLOG #89: (D6, R2) crystal's team detail with mon 0 BOTH "
                "shiny-capable (Atk&2, Def/Spe/Spc=10, g2_dv_shiny's own formula) "
                "AND nicknamed at Lv 100 via --op hofdv x4 + --op hofnick "
                "(tests/host_gbsurgery_tool.c) -- the exact worst "
                "case for the suffix buffer (\" Lv.100 *\" is 9 chars + NUL; the "
                "old char[8] overflowed here, R2); no crash, the shiny mark "
                "renders, and the nickname is trimmed to fit (R4) rather than "
                "running off-screen")
        try:
            sess = run_b89_hof_detail_only(core_mod, image_mod, a.image, a.out, which, name, cap)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b89 hof extra ({a.b89_hof_extra}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name2, reason in skipped:
            print(f"  [skip] {name2}: {reason}")
        return 0

    if a.b194_hof:
        try:
            sess = run_b194_hof(core_mod, image_mod, a.image, a.out, a.b194_hof)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b194 hof ({a.b194_hof}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.b194_hof_no_rom:
        try:
            sess = run_b194_hof_no_rom(core_mod, image_mod, a.image, a.out, a.b194_hof_no_rom)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b194 hof no-rom ({a.b194_hof_no_rom}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.b90_fly:
        try:
            sess = run_b90_fly(core_mod, image_mod, a.image, a.out, a.b90_fly)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b90 fly ({a.b90_fly}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.b94_boxname:
        try:
            sess = run_b90_boxname(core_mod, image_mod, a.image, a.out, a.b94_boxname)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b94 boxname ({a.b94_boxname}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.b132_portrait:
        try:
            sessions = run_b132_portrait(core_mod, image_mod, a.image, a.out, a.b132_portrait)
            for sess in sessions:
                ok += sess.taken
                skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b132 portrait ({a.b132_portrait}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.u4_empty:
        try:
            sess = run_u4_empty(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] u4 empty: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.d7_gold:
        try:
            sess = run_d7_gold(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] d7 gold: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.b86_clock:
        try:
            if a.b86_clock == "crystal":
                sess = run_b86_clock(core_mod, image_mod, a.image, a.out)
            else:
                sess = run_b86_clock_gen1_fallback(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b86 clock ({a.b86_clock}): {e}")
        ran = True
    if a.b87_dex:
        try:
            sess = run_b87_dex(core_mod, image_mod, a.image, a.out, a.b87_dex)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b87 dex ({a.b87_dex}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True
    if a.b64_import:
        try:
            sessions = run_b64_import(core_mod, image_mod, a.image, a.out, a.b64_import)
            for sess in sessions:
                ok += sess.taken
                skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b64 import ({a.b64_import}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True
    if a.b124_sdcount:
        measure_dex_sd_reads(core_mod, image_mod, a.image, a.out)
        ran = True
    if a.b196_sdreads:
        measure_dex_sd_reads(core_mod, image_mod, a.image, a.out, which=a.b196_sdreads)
        ran = True
    if a.b208_dexcache:
        run_b208_dexcache(core_mod, image_mod, a.image, a.out, which=a.b208_dexcache)
        ran = True
    if a.b124_bobcheck:
        run_b124_bobcheck(core_mod, image_mod, a.image, a.out, a.b124_bobcheck)
        ran = True
    if a.b124_dexicons:
        try:
            sess = run_b124_dexicons(core_mod, image_mod, a.image, a.out,
                                     a.b124_dexicons, fallback=a.b124_fallback)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b124 dexicons ({a.b124_dexicons}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True
    if a.b85_daycare:
        try:
            sess = run_b85_daycare(core_mod, image_mod, a.image, a.out, a.b85_daycare)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b85 daycare ({a.b85_daycare}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True
    if a.b114_yard:
        try:
            sess = run_b114_yard(core_mod, image_mod, a.image, a.out, a.b114_yard)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b114 yard ({a.b114_yard}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.b88_flags:
        try:
            sess = run_b88_flags(core_mod, image_mod, a.image, a.out, a.b88_flags)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b88 flags ({a.b88_flags}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.b88_flags_d6:
        try:
            sess = run_b88_flags_d6(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b88 flags d6: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.r1_xfer:
        try:
            sess = run_r1_xfer(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] r1 xfer: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True
    if a.r1_xfer_red:
        try:
            sess = run_r1_xfer_red(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] r1 xfer red: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True
    if a.s150_10:
        try:
            sess = run_s150_10(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-10: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True
    if a.s150_10_two_bad:
        try:
            sess = run_s150_10_two_bad(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-10 two-bad: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True
    if a.s150_10_four_bad:
        try:
            sess = run_s150_10_four_bad(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-10 four-bad: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True
    if a.m1_map:
        try:
            sess = run_m1_map(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] m1 map: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.m3_map_teleport:
        try:
            sess = run_m3_map_teleport(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] m3 map teleport: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.m1_map_vclamp:
        try:
            sess = run_m1_map_vclamp(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] m1 map vclamp: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    # BACKLOG #91 M1-G2: the tiny2 lane's `ran = True` latch (its Item 1, #117)
    # is not present in this dispatch chain as of this landing (grepped for it
    # first) -- added in the existing per-flag `return 0` style below rather
    # than restructuring the chain (that restructuring is tiny2's own commit).
    if a.m1_map_g2:
        try:
            sess = run_m1_map_gen2(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] m1 map g2: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.m1_map_g2_wrong_game:
        try:
            sess = run_m1_map_gen2_wrong_game(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] m1 map g2 wrong game: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.m1_map_g2_no_rom:
        try:
            sess = run_m1_map_gen2_no_rom(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] m1 map g2 no rom: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.gbmon:
        try:
            sess = run_gbmon(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] gbmon: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.b93_menu:
        try:
            sess = run_b93_menu(core_mod, image_mod, a.image, a.out, a.b93_menu)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b93 menu ({a.b93_menu}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.b154_boxmenu:
        try:
            sess = run_b154_boxmenu(core_mod, image_mod, a.image, a.out, a.b154_boxmenu)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b154 boxmenu ({a.b154_boxmenu}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.b187_chains:
        for label, fn in (("A", run_b187_chain_a), ("B", run_b187_chain_b),
                          ("C", run_b187_chain_c)):
            try:
                sess = fn(core_mod, image_mod, a.image, a.out)
                ok += sess.taken
                skipped += sess.skipped
            except RuntimeError as e:
                print(f"  [STOPPED] b187 chain {label}: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if a.b200:
        try:
            for sess in run_b200_chain(core_mod, image_mod, a.image, a.out):
                ok += sess.taken
                skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b200 chain: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if getattr(a, "b199_chain_d", False):
        # BACKLOG #199 (lane b199): append-only, same convention as --b200 above.
        try:
            for sess in run_b199_chain_d(core_mod, image_mod, a.image, a.out):
                ok += sess.taken
                skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b199 chain D: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        ran = True

    if ran:
        return 0

    if a.gbnames:
        # every new flag block sets `ran = True`; lane tiny2's own `ran = False`
        # latch (added right before `if ran: return 0` after this whole chain)
        # returns for all of them -- this block already carries the statement so
        # that merge is trivial (gbnames brief).
        ran = True
        try:
            sess = run_gbnames(core_mod, image_mod, a.image, a.out, a.gbnames)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] gbnames ({a.gbnames}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.s2_bank:
        # #120: new block at the END of the dispatch chain (minimal
        # contact with h118/p2/mapg2/b124, which all edit this file too) -- ran =
        # True per lane tiny2's own latch convention.
        ran = True
        try:
            sess = run_s2_bank(core_mod, image_mod, a.image, a.out, a.s2_bank,
                                clip_rom=a.s2_bank_clip)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s2-bank ({a.s2_bank}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.s2_bank_control:
        # #120 F1: same append-only convention as --s2-bank above.
        ran = True
        try:
            sess = run_s2_bank_control(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s2-bank-control: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if getattr(a, "s2_control", False):
        # #143: same append-only convention as --s2-bank-control above.
        ran = True
        try:
            sess = run_s2_control(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s2-control: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if getattr(a, "s150_2", False):
        # BACKLOG #150 S150-2: same append-only convention as --s2-bank-control above.
        ran = True
        try:
            sess = run_s150_2_bank_native(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-2: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if getattr(a, "s150_3", False):
        # BACKLOG #150 S150-3: same append-only convention as --s150-2 above.
        ran = True
        try:
            sess = run_s150_3_escape_gate(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-3: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if getattr(a, "s150_13", False):
        # BACKLOG #150 S150-13: same append-only convention as --s150-2/--s150-3 above.
        ran = True
        try:
            sess = run_s150_13_carry_badge(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-13: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if getattr(a, "s150_14", False):
        # BACKLOG #150 S150-14: same append-only convention as --s150-2/--s150-3 above.
        ran = True
        try:
            sess = run_s150_14_native_edit(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-14: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if getattr(a, "s150_15", False):
        # BACKLOG #150 S150-15: same append-only convention as --s150-2/--s150-14 above.
        ran = True
        try:
            sess = run_s150_15_view_original(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-15: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if getattr(a, "b166", False):
        # BACKLOG #166: append-only, same convention as --s2-bank-control above.
        ran = True
        try:
            sess = run_b166(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b166: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0
    if getattr(a, "b182", False):
        # BACKLOG #182 (b219): append-only, same convention as --b166 above.
        ran = True
        try:
            sess = run_b182_release_gate(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b182: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0
    if getattr(a, "b190", False):
        # BACKLOG #190: same append-only convention as --s150-2/--s150-3/--s150-14 above.
        ran = True
        try:
            sess = run_b190_move_refusal(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b190: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if getattr(a, "b188", False):
        # BACKLOG #188: same append-only convention as --b190/--s150-14 above.
        ran = True
        try:
            sess = run_b188_resume_cell(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b188: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if getattr(a, "s150_4", False):
        # BACKLOG #150 S150-4/5 follow-up (lane s150-4-5b, BACKLOG #171): same
        # append-only convention as --s150-2/--s150-3/--s150-14 above.
        ran = True
        try:
            sess = run_s150_4_uplift(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-4: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0
    if getattr(a, "s150_7", False):
        # BACKLOG #150 S150-7: append-only, same convention as --s150-2/3/14 above --
        # needs a SECOND image (--s150-7-red), unlike every prior --s150-* flag.
        ran = True
        if not a.s150_7_red:
            sys.exit("--s150-7 also needs --s150-7-red (see --s150-7's own --help)")
        try:
            sess = run_s150_7_down_edge(core_mod, image_mod, a.image, a.s150_7_red, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-7: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0
    if getattr(a, "s150_8", False):
        # BACKLOG #150 S150-8: append-only, same convention as --s150-7 above --
        # single image (no second --s150-7-red-style flag needed).
        ran = True
        try:
            sess = run_s150_8_gen3_arm(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-8: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0
    if getattr(a, "s150_8_party", False):
        # BACKLOG #174 (S150-8c): append-only, same convention as --s150-8 above.
        ran = True
        try:
            sess = run_s150_8_party_vsd(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-8-party: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0
    if getattr(a, "s150_8d_savenow", False):
        # BACKLOG #175 (S150-8d): append-only, same convention as --s150-8-party.
        ran = True
        try:
            sess = run_s150_8d_savenow(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-8d-savenow: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0
    if getattr(a, "s150_9", False):
        # BACKLOG #150 S150-9: append-only, same convention as --s150-8 above.
        ran = True
        try:
            sess = run_s150_9_merge_screen(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-9: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0
    if getattr(a, "s150_9_site2", False):
        # BACKLOG #209: append-only, same convention as --s150-4/--s150-9 above.
        ran = True
        try:
            sess = run_s150_9_site2(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-9-site2: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0
    if getattr(a, "s150_8_bridge", False):
        # BACKLOG #150 S150-8: append-only, same convention as --s150-7 above --
        # needs a SECOND image (--s150-7-red, reused rather than a new flag name).
        ran = True
        if not a.s150_7_red:
            sys.exit("--s150-8-bridge also needs --s150-7-red (see --s150-8-bridge's "
                      "own --help)")
        try:
            sess = run_s150_8_bridge(core_mod, image_mod, a.image, a.s150_7_red, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-8-bridge: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0
    if getattr(a, "s150_12", False):
        # BACKLOG #150 S150-12: append-only, same convention as --s150-4/7/8 above --
        # single image (make delta-gb's own combined image).
        ran = True
        try:
            sess = run_s150_12_copy_edge(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-12: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if getattr(a, "b142", False):
        # BACKLOG #142: append-only, same two-image convention as --s150-7 above.
        ran = True
        if not a.b142_before:
            sys.exit("--b142 also needs --b142-before (see --b142's own --help)")
        try:
            sess = run_b142_tab_focus_arrival(core_mod, image_mod, a.image,
                                               a.b142_before, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b142: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.b54_romhack:
        # BACKLOG #54: same append-only convention as --s2-bank-control above.
        ran = True
        try:
            sess = run_b54_romhack(core_mod, image_mod, a.image, a.out, a.b54_romhack)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] b54-romhack ({a.b54_romhack}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if a.d1_boxname_gate:
        # b199 review D1 fix proof: same append-only convention as --b54-romhack above.
        ran = True
        try:
            sess = run_d1_boxname_gate(core_mod, image_mod, a.image, a.out, a.d1_boxname_gate)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] d1-boxname-gate ({a.d1_boxname_gate}): {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    if getattr(a, "s150_11", False):
        # BACKLOG #150 S150-11: append-only, same delta-gb combined-image convention
        # as --s150-12 above (needs a Game Boy session too, not just the Gen-3 side).
        ran = True
        try:
            sess = run_s150_11_reconcile(core_mod, image_mod, a.image, a.out)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] s150-11: {e}")
        _write_manifest(a.out, ok, skipped)
        print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
        for name, reason in skipped:
            print(f"  [skip] {name}: {reason}")
        return 0

    try:
        sess = run_standalone(core_mod, image_mod, a.image, a.out)
        ok += sess.taken
        skipped += sess.skipped
    except RuntimeError as e:
        print(f"  [STOPPED] boot-picker/standalone: {e}")
    try:
        sess = run_nav_gb(core_mod, image_mod, a.image, a.out)
        ok += sess.taken
        skipped += sess.skipped
    except RuntimeError as e:
        print(f"  [STOPPED] nav-gb refusal: {e}")

    _write_manifest(a.out, ok, skipped)

    print(f"\n{len(ok)} shot(s), {len(skipped)} skip(s)")
    for name, reason in skipped:
        print(f"  [skip] {name}: {reason}")
    return 0


def run_gbmon(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #92: the new ITEM row on the Gen-1/2 mon-menu popup
    (app_mon_menu_readonly, pdna_main.c) -- Gen 2 only. `rom` must be a Gen-2-only
    fused image (Gold.gbc+Gold.sav or Crystal.gbc+Crystal.sav, tools/fuse_gb.py,
    ONE ROM per image -- BACKLOG #98's fused-image-by-generation harness gap, same
    constraint run_d7_gold()/run_u4_bag() already document).

    A single-ROM fused image has no Emerald/Gen-3 save fused in, so
    gb_delta_boot_pick()'s own `if (n == 1) return 0` (pdna_main.c ~8913) skips the
    boot picker entirely -- exactly run_d7_gold()'s own nav, ONE tap (A -> box grid,
    #279: no info page), not the DOWN+A+A a combined multi-ROM image needs.

    Guy's own roms/gb corpus has every box on every save completely full (a
    "living dex" test save -- run_standalone()'s own doc comment), so the box
    grid's default cursor position (top-left) is always occupied; no navigation
    is needed before the first A.

    Row order verified here matches BACKLOG #92's brief: VIEW/EDIT, ITEM,
    LEGALITY, MOVE TO BOX, COPY, RELEASE, CANCEL -- ITEM sits where Gen 3's own
    A_ITEM does (right after the summary row), not where the pre-#92 comment in
    app_mon_menu_readonly (pdna_main.c) said Gen 3 "has no separate Item row" to
    make room for -- that comment is now stale for a Gen-2 mount specifically
    (updated alongside this row, not left to drift)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "gbmon_")
    print("== BACKLOG #92: the Gen-2 mon-menu ITEM row ==")

    boot_to_gb_session(s, rom)   # single-ROM image, no boot picker -- same nav as d7_gold
    s.shot("01_box_grid", "#92: box grid, top-left cell occupied (this corpus's "
                           "every box is full) -- the mon this run's ITEM row "
                           "edits")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # occupied cell -> its menu
    s.shot("02_mon_menu", "#92: the Gen-2 mon-menu popup now reads VIEW/EDIT, "
                           "ITEM, LEGALITY, MOVE TO BOX, COPY, RELEASE, CANCEL -- "
                           "ITEM is the NEW row (AppSrcOps.item / gb_item_hook, "
                           "k_gb_ops_gen2), sitting right after VIEW/EDIT exactly "
                           "where Gen 3's own A_ITEM sits in app_mon_menu's order")

    s.tap("DOWN", settle=gb_shots.SETTLE)                   # VIEW/EDIT (row 0) -> ITEM (row 1)
    s.shot("03_item_row_selected", "#92: cursor on the ITEM row")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # -> pick_item(), restricted (1..255, "#n")
    s.shot("04_item_picker", "#92: gb_item_hook opens the SAME pick_item() screen "
                              "app_quick_item (Gen 3) uses, restricted to ids "
                              "1..255 shown as \"#n\" via pick_item_set_gen1_2_max "
                              "-- the identical restricted mode gb_editor.c's own "
                              "GBE_ITEM row already uses inside the full summary "
                              "editor, now reachable straight from the mon menu too")

    s.tap("DOWN", settle=gb_shots.SETTLE)                   # move off the current selection
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # confirm -> gb_set_held_item + gb_edit_commit
                                                               # (steps 3-5, the same load/commit path EDIT
                                                               # uses) -> gb_persist()'s PDNA_DELTA branch
    s.shot("05_delta_refusal", "#92: gb_edit_commit's step 5 (gb_persist) hits the "
                                "SAME PDNA_DELTA in-session-only branch run_standalone's "
                                "own D2/D5 shots (06/13) do -- 'Edits are in-session "
                                "only in the emulator build.' The write DID land in "
                                "EWRAM (pristine is re-baselined right here, same as "
                                "D2's fix); nothing is lost, just not persisted to a "
                                "card that does not exist under mGBA")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -> back at the box grid
    s.shot("06_back_at_grid", "#92: dismissing the refusal returns to the box grid, "
                               "re-paged (g_m->loaded = -1 forces the reload, same "
                               "as every other GB write path)")

    # A short idle run before reopening the menu (past the re-page's own repaint) --
    # without it, a second A right after the dismiss-A landed on the SAME popup
    # again instead of selecting VIEW/EDIT (a settle-timing quirk against this
    # specific state, not a #92/#95 defect -- isolated by hand: the identical two
    # A-taps work first time, straight off the box grid, with no picker/refusal
    # cycle in between).
    s.run(120)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # same cell -> menu again
    s.shot("07_menu_holding_item", "#92: opening the cell's menu again already "
                                    "confirms the write on its own -- the header "
                                    "now reads 'Converted copy / Holding an item' "
                                    "(it read plain 'Converted copy' in shot 02, "
                                    "before ITEM was used) -- gb_item_hook's write "
                                    "landed on the in-EWRAM record, not just on the "
                                    "picker's own display")

    s.run(60)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # VIEW/EDIT -> the native summary
    s.shot("08_summary_item_confirmed", "#92: VIEW/EDIT opens the native GB summary, "
                                         "where the SAME record's Item field (INFO "
                                         "panel) now reads '#2' -- the exact id picked "
                                         "in shot 04 -- confirming the mon-menu row's "
                                         "write landed on the record, not just on the "
                                         "picker's own display")

    # -------------------------------------------------------------------------
    # BACKLOG #95: the summary-field parity audit's closed gaps -- Shiny, Egg, and
    # Met Time/Level/Loc/OT Gender, all new GBE_* rows in gb_editor.c reachable from
    # here via SELECT (pdna_gbedit.c's flat field-list editor, the reviewed
    # fallback BACKLOG #41 already documents).
    # -------------------------------------------------------------------------
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # summary -> the flat editor
    # A generous idle pad before the first input on this screen -- without it, the
    # first DOWN (and only the first) silently did not move the cursor, a timing
    # quirk against this specific "picker -> refusal -> re-page -> menu -> summary
    # -> SELECT" run of screens (isolated by hand: the SAME editor, reached by a
    # plain two-tap A,A + SELECT off a fresh box grid, takes ordinary SETTLE-length
    # DOWNs with no pad needed at all -- see this file's own git history for the
    # isolation). Not a #92/#95 defect; every DOWN below gets the same larger pad
    # to stay safely inside whatever margin actually fixed it.
    s.run(200)
    s.shot("09_flat_editor_top", "#95: pdna_gbedit.c's flat field-list editor over "
                                  "the SAME record -- gbe_fields() drives every row "
                                  "generically, so the six new rows this slice adds "
                                  "appear here with zero screen-side code")

    # ---------------------------------------------------------------------------
    # gbmon C4 reshoot: the Mail confirm. ITEM is row 4 (NICK=0,OT=1,OTID=2,LEVEL=3,
    # ITEM=4) -- a short detour off row 0, back to row 0 before the 29-DOWN scroll
    # below (which is calibrated to start there). item_build()'s restricted-mode
    # search is a NUMERIC PREFIX match (source/pdna_pick.c, item_build's own
    # comment), so typing "158" through osk_search finds item id 158 (G2_MAIL_FLOWER,
    # source/gb_session.c) directly rather than paging one id at a time. Declined
    # (B = no, source/pdna_main.c's app_confirm) so the record's held item stays at
    # #2 (set back in shot 05) -- an ACCEPT here would zero out gb_set_egg's own
    # "no item" precondition for the later Egg-refusal shot below, which depends on
    # the item staying non-zero.
    # ---------------------------------------------------------------------------
    s.press_n("DOWN", 4, settle=60)                         # NICK (0) -> ITEM (4)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # -> pick_item(), current = #2
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # -> osk_search
    s.tap("A", settle=gb_shots.SETTLE)                      # type '1' (row0 col0, no seed to clear)
    s.press_n("RIGHT", 4, settle=gb_shots.SETTLE)           # col0 '1' -> col4 '5'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '5' -> field "15"
    s.press_n("RIGHT", 3, settle=gb_shots.SETTLE)           # col4 '5' -> col7 '8'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '8' -> field "158"
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm search -> filtered to id 158
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # select id 158 (G2_MAIL_FLOWER) ->
                                                              # gbs_is_mail_item(158) true, no Egg yet
                                                              # -> app_confirm(PDNA_GBEDIT_MAIL_TITLE)
    s.shot("mail_confirm", "#95: gbmon C4 reshoot: picking a Mail id (158, G2_MAIL_FLOWER) "
                            "on the ITEM row triggers app_confirm(PDNA_GBEDIT_MAIL_TITLE, "
                            "PDNA_GBEDIT_MAIL_L1) -- 'SET THIS MAIL ITEM? / No mailbox: "
                            "locks Move/Release.' -- since this tree tracks no mailbox "
                            "(gbs_is_mail_item, gb_session.h)")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # decline -> item unchanged (#2)
    s.shot("mail_declined", "#95: gbmon C4 reshoot: B declines -- back on the ITEM row, "
                             "unchanged (still '#2' from shot 05); a declined confirm "
                             "must not silently set the item anyway")
    s.press_n("UP", 4, settle=60)                           # ITEM (4) -> NICK (0), back where
                                                              # the 29-DOWN scroll below expects
                                                              # to start

    # NICK,OT,OTID,LEVEL,ITEM,FRIEND (6) + MV0-3 (4) + PPU0-3 (4) + PP0-3 (4) +
    # DVA,DVD,DVS,DVC,DVH (5) + GENDER (1, Bulbasaur has a real gender ratio) = 24
    # rows before SHINY -- 29 DOWNs lands on Met OT Gender (row 29), scrolling the
    # 16-row window to show DVA..Met OT Gender (rows 14-29) in one screen: every
    # new row from this slice, in the SAME shot. Taken in THREE chunks, each ending
    # in a real shot() call -- one long chunk of 29 raw taps with no shot() in
    # between reliably lands back on row 0 against this exact multi-screen run (a
    # harness quirk isolated by hand: a shot() call between chunks fixes it every
    # time, on this same image, same history, same everything else -- not a
    # #92/#95 product defect; the flat editor's own row model is independently
    # proven by host_gbeditor_test.c's 461,230 checks over real saves).
    s.press_n("DOWN", 10, settle=60)
    s.shot("10a_scrolling", "#95: 10 DOWNs in -- cursor on Max PP 1, still well "
                             "above the new rows")
    s.press_n("DOWN", 10, settle=60)
    s.shot("10b_scrolling", "#95: 20 DOWNs in -- cursor on DV Spe, Gender/Shiny/Egg/"
                             "Met just a few rows further down")
    # gbmon C11 reshoot: the four Met rows (gb_editor.c's gbe_fields) are gated on
    # e->has_caught, which gb_mark_caught (source/pdna_gen12.c) only ever sets true
    # for a Crystal session (gb_session_is_crystal) -- Gold/Silver never gets them.
    # "gold" appears in every Gold-only fused image this script builds and nowhere
    # in a Crystal one (crystal_only.gba / crystal_forced.gba), so this is an exact
    # discriminator for THIS harness's own naming convention, not a guess.
    is_crystal = "gold" not in rom.name.lower()
    s.press_n("DOWN", 9, settle=60)
    if is_crystal:
        s.shot("10_new_rows_visible", "#95: C11 reshoot: 29 DOWNs in -- Shiny and Egg "
                                       "both 'No', and the four Met fields VISIBLE, "
                                       "carrying this real Crystal.sav mon's own "
                                       "capture record (Met Time 'Morning', a level, "
                                       "Met Loc '#16', Met OT Gender 'M') -- decoded, "
                                       "not invented, sitting right where Gen 3's own "
                                       "F_SHINY and F_MET* rows sit in pdna_edit.c's "
                                       "row order. gb_mark_caught set has_caught=true "
                                       "for this Crystal session (gb_session_is_crystal)")
    else:
        s.shot("10_new_rows_visible", "#95: C11 reshoot: 29 DOWNs in on a GOLD-only image -- "
                                       "count the rows: Shiny, Egg, then StatExp HP/"
                                       "Atk/Def/Spe -- the four Met rows are ABSENT. "
                                       "gb_mark_caught (source/pdna_gen12.c) never sets "
                                       "has_caught true for a Gold/Silver session "
                                       "(gb_session_is_crystal returns false), and "
                                       "gbe_fields() (gb_editor.c) skips GBE_METTIME/"
                                       "METLEVEL/METLOC/METOTGENDER outright when "
                                       "!e->has_caught -- the C11 fix's own point: a "
                                       "Gen-2 target no longer gets a capture record "
                                       "just because it is Gen 2")

    # This fused image's top-left cell may or may not be the Atk-DV-forced-female
    # Bulbasaur (crystal_forced.gba only, built specifically so this species' 7:1-
    # male ratio has NO shiny candidate in its current gender -- see gbmon's own
    # BACKLOG #95 item 4 brief). Every OTHER fused image (Gold, the plain Crystal
    # corpus) has this same box-0-slot-0 Bulbasaur at its REAL corpus gender
    # (already male in both, so Shiny ON keeps it male trivially -- no popup at
    # all): detect by filename, per this file's own d7_gold precedent, rather
    # than probing the frame, so the two paths cannot silently diverge on a typo.
    forced_gender_image = "forced" in rom.name.lower()
    s.press_n("UP", 5, settle=60)                           # Met OT Gender (29) -> Shiny (24)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # toggle ON
    if forced_gender_image:
        s.shot("11_shiny_on", "#95: gbmon C3 reshoot: this fused image's top-left cell is "
                               "Bulbasaur with Atk DV forced to 1 (gender ratio 31, 7:1 "
                               "male) -- currently FEMALE, and none of the 8 shiny Atk-DV "
                               "candidates {2,3,6,7,10,11,14,15} is in the female range "
                               "(dv<=1) at that ratio, so A on Shiny is FORCED to move "
                               "gender: gbe_flip_shiny() sets shiny_gender_forced and "
                               "pdna_gbedit.c's gbedit_shiny_forced_note() pops the "
                               "PDNA_GBEDIT_SHINY_FORCED_TITLE/MALE_L1/L2 message "
                               "immediately ('GENDER FORCED / No shiny female exists for "
                               "this species; it is now male.') -- gb_editor.c's own C2 "
                               "case, now shown live instead of only host-tested")
        s.tap("A", settle=gb_shots.BIG_SETTLE)              # dismiss the forced-gender popup
        s.shot("11b_shiny_on_dismissed", "#95: gbmon C3 reshoot: A dismisses the popup, back "
                                          "on the flat editor -- Shiny now reads 'Yes' and "
                                          "the Gender row above it (not shown in this crop) "
                                          "now reads male, matching the forced result")
    else:
        s.shot("11_shiny_on", "#95: A on Shiny flips it to 'Yes' -- this corpus mon is "
                               "already male and a male shiny Atk-DV candidate exists "
                               "(gbe_flip_shiny() keeps the current gender when it can), "
                               "so no forced-gender popup fires here -- see the gbmon "
                               "C3 reshoot's own crystal_forced image for that case, "
                               "built with Atk DV set to 1 specifically so no shiny "
                               "candidate shares this 7:1-male species' female gender")

    s.tap("DOWN", settle=60)                                # Shiny -> Egg
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # toggle ON -- refused: this record
                                                              # already holds item #2 (shot 05)
    s.shot("12_egg_refused", "#95: gbmon C10 reshoot: A on Egg is REFUSED, not silently "
                              "ignored -- this record already holds item #2 (planted "
                              "back in shot 05 via the mon-menu ITEM row), and "
                              "gb_set_egg(e, true) refuses outright while the held-item "
                              "field is non-zero (review C5, gb_edit.c). C10 wires the "
                              "refusal to a real message: gbedit_adjust_refused's "
                              "GBE_EGG case pops PDNA_GBEDIT_EGG_ITEM_TITLE/L1 -- 'EGG "
                              "CAN'T HOLD ITEMS / Remove the held item first.' -- the "
                              "SAME message pdna_gbedit.c's GBE_K_ITEM branch shows for "
                              "the other direction (a non-zero item picked while the "
                              "record IS an Egg), reused rather than duplicated")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -- Egg never moved,
                                                              # cursor still on the Egg row
    s.shot("12b_egg_refused_dismissed", "#95: gbmon C10 reshoot: A dismisses the message; "
                                         "back on the flat editor with Egg still 'No' -- "
                                         "the refusal left the record untouched, exactly "
                                         "like every other GBE_K_NUM refusal in this row")

    if is_crystal:
        s.tap("DOWN", settle=60)                            # Egg -> Met Time
        s.tap("RIGHT", settle=60)                           # None -> Morning
        s.tap("RIGHT", settle=60)                           # Morning -> Day
        s.tap("RIGHT", settle=60)                           # Day -> Night
        s.tap("DOWN", settle=60)                            # Met Time -> Met Level
        s.press_n("RIGHT", 5, settle=60)                    # Met Level 0 -> 5
        s.shot("13_met_edited", "#95: Met Time cycled to 'Night' and Met Level bumped "
                                 "by +5 (RIGHT x5) to whatever this real Crystal.sav "
                                 "record's own original level was + 5 -- gb_set_caught() "
                                 "repacking bytes 0x1D/0x1E one field at a time via the "
                                 "new gb_get_caught_* readers, the same 'read the other "
                                 "three, write all four back' shape Gen 3's own "
                                 "F_METLEVEL/F_METGAME rows use over metLocation/metGame")
    else:
        # No Met rows exist to edit on Gold -- the row right after Egg is StatExp HP
        # (gbe_fields()'s next entry once METTIME..METOTGENDER are skipped). Edited
        # anyway (same DOWN/RIGHT shape) so this run still ends on a real edited-field
        # shot rather than stopping short, and to keep this function's tap count
        # identical between the two branches (only the CAPTION differs, matching what
        # is actually on screen -- see the C11 reshoot comment above shot 10).
        s.tap("DOWN", settle=60)                            # Egg -> StatExp HP
        s.press_n("RIGHT", 3, settle=60)
        s.tap("DOWN", settle=60)                            # StatExp HP -> StatExp Atk
        s.press_n("RIGHT", 5, settle=60)
        s.shot("13_statexp_edited", "#95: C11 reshoot: with no Met rows to land on, the "
                                     "same DOWN/RIGHT taps instead land on StatExp HP "
                                     "then StatExp Atk (gbe_fields()'s next entries "
                                     "after Egg on a Gold session) -- proof the row "
                                     "list genuinely reflows around the absent Met "
                                     "rows rather than leaving a gap or a stale cursor")
    return s


def run_b154_boxmenu(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #154: the box-options menu's Rename row used to draw and act
    unconditionally, refusing on A with 'NO BOX NAMES' when the source has no
    box-name table at all (Gen 1: gbbn_supported()==false) -- the shown-then-
    refused pattern the repo's own convention is to avoid. The fix (pdna_box.c
    box_options_menu, show_rename computed once before the loop) OMITS the row
    entirely on such a source instead: Gen 1 (`which == "red"` or "yellow") sees
    4 rows (Wallpaper/Export all .pk/Release all/Cancel); Gen 2 (`which ==
    "gold"`) is unaffected -- still 5 rows, Rename box first, byte-identical to
    before this fix (can_rename() is true there, gbbn_supported()==true).

    --image MUST be a ONE-ROM fused image matching `which` (tools/fuse_gb.py,
    no Emerald.sav -- same BACKLOG #98 reasoning run_b93_menu's own docstring
    gives), so boot_to_gb_session() skips the picker."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b154_{which}_")
    print(f"== BACKLOG #154: box-options menu Rename row, gated on can_rename ({which}) ==")

    boot_to_gb_session(s, rom, which=which)
    s.tap("UP", settle=100)                  # occupied cell -> TITLE row
    s.tap("SEL", settle=100)                 # SELECT on title -> box_options_menu

    if which in ("red", "yellow"):           # Gen 1: no box-name table at all
        s.shot("01_box_menu_no_rename", "BACKLOG #154: Gen 1's box menu -- "
               "'Rename box' is OMITTED (was: drawn then refused with 'NO BOX "
               "NAMES' on A) -- 'Wallpaper / Export all .pk / Release all / "
               "Cancel', 4 rows, cursor defaults to Wallpaper (row 0 now)",
               claim=["Wallpaper", "Export all .pk", "Release all", "Cancel"],
               claim_absent=["Rename box"])
    else:                                     # Gen 2: has a real box-name table
        s.shot("01_box_menu_has_rename", "BACKLOG #154: Gen 2's box menu is "
               "UNCHANGED by this fix -- 'Rename box' is still the first row "
               "(can_rename() is true here, gbbn_supported()==true) -- "
               "'Rename box / Wallpaper / Export all .pk / Release all / "
               "Cancel', 5 rows, byte-identical to before",
               claim=["Rename box", "Wallpaper", "Export all .pk", "Release all",
                      "Cancel"])
    return s


def run_b93_menu(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #93: DUPLICATE / TO DAY-CARE / EXPORT .pk on the read-only mon menu,
    plus the GB box menu's own EXPORT ALL / RELEASE ALL (can_boxops/export_all/
    release_all). `rom` MUST be a ONE-ROM fused image matching `which` (Gold.gbc+
    Gold.sav or Red.gb+Red.sav, no Emerald.sav -- same BACKLOG #98 single-ROM
    reasoning run_gbmon()/run_s2_bank() already document) so boot_to_gb_session()
    skips the picker.

    Row order verified by hand against this exact image (probe screenshots, not
    guessed), matching Gen 3's own app_mon_menu occupied-mon order (...COPY,
    DUPLICATE, TO DAY-CARE, EXPORT, RELEASE): Gen 2 (gold) = VIEW/EDIT, ITEM,
    LEGALITY, MOVE TO BOX, COPY, DUPLICATE, TO DAY-CARE, EXPORT .pk, RELEASE, CANCEL
    (10 rows, PDNA_ROMENU_MAX); Gen 1 (red) is the same list minus ITEM (9 rows) --
    DUPLICATE sits 5 DOWNs in on gold, 4 on red. D8 correction (review-opus,
    2026-09-15): "all fit on screen with no windowing" was WRONG -- ui_popup_vfit
    gives vis=8 for this panel's geometry, so on gold RELEASE and CANCEL scroll off
    the bottom of the visible list (parity-correct: Gen 3's own app_mon_menu windows
    the same way once it has enough rows). Confirmed by re-reading shot 02 itself,
    not re-derived from a claim.

    THREE EMULATOR-BOUNDARY FACTS, each verified by hand against this exact image,
    not assumed:
      1. D8 correction (review-opus, 2026-09-15): "EVERY box AND the party are at
         capacity" was WRONG -- that was true of box 0 and the party specifically
         (the ones this run actually starts on: box 0 20/20, party 6/6, both saves),
         generalized past what was actually checked. A full box census (review-
         opus) finds real room elsewhere: Red box 4 (19/20), box 5 (16/20); Yellow
         box 7 (11/20), box 8 (7/20), box 9 (1/20), boxes 10-11 (0/20); Gold boxes
         12-13 (17/20 each); Crystal box 12 (18/20). The REAL, ALWAYS-true reason
         DUPLICATE's landing-slot success message (decision 1) is unshootable here
         is fact 2 below, not corpus fullness: gb_persist("dup") refuses under
         PDNA_DELTA regardless of whether gbs_insert() itself would have succeeded,
         so even a box with room would still dead-end at the same wall fact 2
         documents, one step later. "EDIT REFUSED / that box is full" (shot 04,
         box 0) is real and correct for THAT box, just not proof that no box in
         this corpus has room -- deferred to hardware either way (HW-QUEUE GBMON-1).
      2. TO DAY-CARE and RELEASE ALL both end in gb_persist(), which hits the SAME
         PDNA_DELTA "Edits are in-session only in the emulator build." wall
         run_gbmon()'s own shot 05 documents -- there is no SD card under mGBA. Both
         edits DO land in g_ed->img regardless (gb_persist's PDNA_DELTA branch
         re-baselines pristine to the post-edit image on purpose, same #62 D2
         posture every other GB write path takes) -- visible in the box grid's own
         occupancy count right after dismissing the wall, captured below.
      3. EXPORT does not call gb_persist (it writes a NEW file, never a save byte).
         app_can_edit() is unconditionally true under PDNA_DELTA (no flashcart gate
         there), so it reaches sf_write_verified, which fails at the FatFs layer
         (no volume mounted under mGBA) with "EXPORT FAILED / open failed" -- the
         real, hardware-shaped file write is HW-QUEUE GBMON-3/4."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b93_{which}_")
    print(f"== BACKLOG #93: DUPLICATE / TO DAY-CARE / EXPORT .pk + the GB box menu ({which}) ==")

    boot_to_gb_session(s, rom, which=which)
    s.shot("01_box_grid", f"BACKLOG #93: {which}'s box grid, freshly entered -- "
                           "top-left cell occupied (this box, box 0, is 20/20 full; "
                           "not every box in this corpus is, see the docstring's D8 "
                           "correction)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # occupied cell -> mon menu
    s.shot("02_mon_menu", f"BACKLOG #93: the {which} mon-menu popup -- "
                          + ("VIEW/EDIT, ITEM, LEGALITY, MOVE TO BOX, COPY, DUPLICATE, "
                             "TO DAY-CARE, EXPORT .pk visible (10 rows total, "
                             "PDNA_ROMENU_MAX, ui_popup_vfit windows to vis=8 -- "
                             "RELEASE/CANCEL scroll off the bottom, parity-correct: "
                             "Gen 3's own app_mon_menu windows the same way)"
                             if which == "gold" else
                             "VIEW/EDIT, LEGALITY, MOVE TO BOX, COPY, DUPLICATE, "
                             "TO DAY-CARE, EXPORT .pk, RELEASE visible (9 rows total "
                             "-- no ITEM on Gen 1 -- windowed to vis=8, CANCEL scrolls "
                             "off the bottom, same parity-correct windowing)")
                          + " -- DUPLICATE/TO DAY-CARE/EXPORT .pk are the three new "
                            "rows, in Gen 3's own occupied-mon relative order")

    downs_to_dup = 5 if which == "gold" else 4
    s.press_n("DOWN", downs_to_dup, settle=gb_shots.SETTLE)
    s.shot("03_duplicate_selected", "BACKLOG #93: cursor on DUPLICATE")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # -> gbs_insert -> GBS_ERR_FULL (this corpus)
    s.shot("04_duplicate_box_full", "BACKLOG #93: gb_dup_hook's gbs_insert() refuses "
                                     "-- 'EDIT REFUSED / that box is full / Save "
                                     "unchanged.' -- THIS box (box 0) really is 20/20 "
                                     "full, so the capacity gate is real here, not a "
                                     "stand-in for a different failure; other boxes in "
                                     "this corpus DO have room (docstring's D8 "
                                     "correction), but DUPLICATE's landing-slot "
                                     "success message would still be unreachable "
                                     "there too -- gb_persist('dup') refuses under "
                                     "PDNA_DELTA regardless (fact 2)")
    s.tap("A", settle=200)                                  # dismiss -> EVERY row's A-press exits the
                                                              # popup back to the box grid regardless of
                                                              # outcome (app_mon_menu_readonly's switch
                                                              # cases all `return` -- verified by hand:
                                                              # this is NOT a re-drawn menu)
    s.shot("05_grid_after_dup_refusal", "BACKLOG #93: dismissing the refusal exits "
                                         "the popup back to the box grid (every row's "
                                         "A-press does this, success or not -- "
                                         "app_mon_menu_readonly's switch cases all "
                                         "`return`) -- unchanged, still 20/20, "
                                         "Bulbasaur still top-left")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # same cell -> menu again
    s.press_n("DOWN", downs_to_dup + 1, settle=gb_shots.SETTLE)   # -> TO DAY-CARE
    s.shot("06_daycare_selected", "BACKLOG #93: cursor on TO DAY-CARE")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # -> app_confirm
    s.shot("07_daycare_confirm", "BACKLOG #93: 'Send to Day-Care?' / 'Moves this "
                                  "Pokemon there.' -- Gen 3's own app_to_daycare "
                                  "strings (pdna_main.c), reused verbatim")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # confirm -> gbd_deposit -> gbs_delete -> gb_persist
    s.shot("08_daycare_delta_wall", "BACKLOG #93: gbd_deposit() lands the mon in "
                                     "RAM (it calls gbs_finish() itself) and the "
                                     "source slot is deleted, THEN gb_persist"
                                     "('daycare-put') hits the PDNA_DELTA wall -- "
                                     "'LEFT AT DAY CARE' and the Day-Care page "
                                     "(the hook's own trailing pdna_gbdaycare() "
                                     "open) are HARDWARE-ONLY")
    s.tap("A", settle=250)                                  # dismiss -> box grid re-pages
    s.shot("09_grid_after_daycare", "BACKLOG #93: back at the box grid -- one fewer "
                                     "occupant than shot 01 (the top-left cell's "
                                     "former occupant is gone, the NEXT mon has "
                                     "taken its place) -- proof the deposit+delete "
                                     "landed in g_ed->img despite the persist "
                                     "refusal, same in-session posture every other "
                                     "GB write path takes")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # the new top-left cell -> its own menu
    s.press_n("DOWN", downs_to_dup + 2, settle=gb_shots.SETTLE)   # -> EXPORT .pk
    s.shot("10_export_selected", "BACKLOG #93: cursor on EXPORT .pk")
    s.tap("A", settle=150)                                  # -> gb_pk_pack -> sf_write_verified
    s.shot("11_export_no_sd", "BACKLOG #93: gb_export_hook reaches sf_write_verified "
                               "(app_can_edit() is unconditionally true under "
                               "PDNA_DELTA) -- 'EXPORT FAILED / open failed', the "
                               "honest FatFs-layer result of no SD volume under "
                               "mGBA; the real .pk1/.pk2 write is HW-QUEUE GBMON-3")
    s.tap("A", settle=200)                                  # dismiss -> box grid

    s.tap("UP", settle=100)                                 # cell -> TITLE row (on_title = true)
    s.shot("12_title_selected", "BACKLOG #93: UP selects the TITLE row")
    s.tap("SEL", settle=100)                                # SELECT on title -> box_options_menu
    s.shot("13_box_menu", "BACKLOG #93: SELECT on the title now opens the box menu "
                           "at all -- BEFORE this lane it was UNREACHABLE on a GB "
                           "grid (gbsrc_can_edit() is hardwired false and can_lift "
                           "is still NULL, the finding that shapes gbsrc_can_"
                           "boxops) -- 'Rename box / Wallpaper / Export all .pk / "
                           "Release all / Cancel', can_boxops() gating the whole "
                           "menu open, not can_rename/can_lift/can_edit")

    # TAKE 2 (BACKLOG #156, the two-BOUNCE retire): everything from here on was
    # re-derived from a live per-tap trace (/tmp/drive_b93.py against this exact
    # image), not assumed -- the previous cut's shots 15-22 were all misrouted (the
    # reviewer's own finding: SELECT cancels the OSK to the PLAIN BOX GRID, source/
    # osk.c:197, not back to the box-options menu; every step downstream of that
    # wrong assumption drifted onto the wrong screen).

    # D2: Rename box attempt (shows input interface) -- unchanged, still correct.
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on Rename box -> osk_core()
    s.shot("14_rename_box_interface", "BACKLOG #156 D2: A on Rename box -- box name OSK "
                                       "with QWERTY layout ('BOX NAME' / 'BOX1'). SELECT "
                                       "(osk.c:197) cancels.")

    # Live trace correction: SELECT does NOT return to the box-options menu -- it
    # drops straight to the PLAIN BOX GRID (cursor still on the title row, footer
    # 'L/R A name SEL menu'). Captured explicitly so this is evidence, not a claim.
    s.tap("SEL", settle=100)                                # SELECT -> cancels OSK -> plain grid
    s.shot("15_osk_cancel_to_plain_grid", "BACKLOG #156 (review correction): SELECT on "
                                           "the OSK does NOT return to the box-options "
                                           "menu -- it drops to the PLAIN BOX GRID "
                                           "(BOX1 19/20, cursor still on the title row). "
                                           "The previous cut of this script assumed a "
                                           "return to the box menu here; every shot after "
                                           "it was misrouted as a result.")

    # SELECT again, straight from this plain-grid state (cursor already on the title
    # row -- no UP needed here, unlike shot 12/13's first approach from a mon cell),
    # reopens the SAME box-options menu.
    s.tap("SEL", settle=100)                                # SELECT on title -> box_options_menu again
    s.shot("16_box_menu_reopened", "BACKLOG #156: SELECT from the plain grid's title "
                                    "row reopens the same box menu ('Rename box / "
                                    "Wallpaper / Export all .pk / Release all / "
                                    "Cancel') -- the door back in, now that D2's OSK "
                                    "visit is done.")

    # D3: Wallpaper. Live trace correction: A on Wallpaper does NOT open a selection
    # grid first -- source/pdna_gbbox_menu.c's Wallpaper hook refuses IMMEDIATELY
    # (pdna_gbnames_on is false for this image), one screen, not two.
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # move to Wallpaper option
    s.shot("17_wallpaper_selected", "BACKLOG #156 D3: cursor on Wallpaper in the "
                                     "(reopened) box menu")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A -> immediate refusal, no selector grid
    s.shot("18_wallpaper_refusal", "BACKLOG #156 D3 (review correction): A on "
                                    "Wallpaper shows the refusal DIRECTLY -- 'NO "
                                    "WALLPAPER / Game Boy boxes have no wallpaper to "
                                    "change. / Press A' -- there is no intermediate "
                                    "wallpaper-selection grid on this build; the "
                                    "previous cut's '16_wallpaper_interface' shot (a "
                                    "grid, refusal deferred to a later confirm) never "
                                    "existed on this image.")
    s.tap("A", settle=100)                                  # dismiss -> plain box grid (BOX1)

    # D5: Party pseudo-box DUPLICATE refusal -- reached for real this time (the
    # previous cut's own comment admitted this was "structural verification" only;
    # live-traced now). L from box 0's plain grid wraps to the LAST box, the party
    # pseudo-box (gb_box_is_party's own nboxes-1 rule, pdna_gen12.c/gb_edit.c:172) --
    # confirmed live: the header reads "GB PARTY 6/6", not a numbered box. The cursor
    # lands on the TITLE row (A there opens the SAME rename OSK D2 used, not a mon
    # menu -- also confirmed live), so DOWN once first, onto the party's own slot 0
    # (a real occupied mon, not empty), before A opens its menu.
    s.tap("L", settle=150)                                  # box grid -> wraps to the party pseudo-box
    s.shot("19_party_pseudo_box", "BACKLOG #156 D5: L from BOX1's plain grid wraps "
                                   "to 'GB PARTY 6/6' -- the party pseudo-box, cursor "
                                   "on its title row")
    s.tap("DOWN", settle=60)                                # title -> party slot 0 (occupied)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the occupied party slot -> its mon menu
    s.press_n("DOWN", downs_to_dup, settle=gb_shots.SETTLE) # -> DUPLICATE (same row order as the box menu)
    s.shot("20_party_duplicate_selected", "BACKLOG #156 D5: cursor on DUPLICATE, in "
                                           "the party pseudo-box's own occupied-mon "
                                           "menu (same row order as an ordinary box "
                                           "cell's)")
    s.tap("A", settle=150)                                  # -> gb_dup_hook -> gb_box_is_party() refuses
    s.shot("21_party_duplicate_refusal", "BACKLOG #156 D5: gb_dup_hook's "
                                          "gb_box_is_party(gen, box) gate refuses -- "
                                          "'CAN'T / Can't duplicate a party mon. / "
                                          "Press A' (pdna_gen12.c:1509/1522, "
                                          "pdna_layout.h:720), captured BEFORE "
                                          "dismissing, live-traced (not the previous "
                                          "cut's structural-proof-only claim)")
    s.tap("A", settle=200)                                  # dismiss -> back to the party pseudo-box grid
    s.tap("R", settle=150)                                  # party pseudo-box -> wraps back to BOX1 19/20

    s.tap("UP", settle=100)                                 # BOX1 grid, cell -> title row
    s.tap("SEL", settle=100)                                # title -> box menu (BOX1 context, for Release all)
    s.press_n("DOWN", 3, settle=gb_shots.SETTLE)            # Rename/Wallpaper/Export -> Release all
    s.shot("22_release_all_selected", "BACKLOG #93: back on BOX1's own box menu "
                                       "(after the D5 party detour) -- cursor on "
                                       "Release all")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # -> app_confirm with the count
    s.shot("23_release_all_confirm", "BACKLOG #93: 'Release all 19 Pokemon?' / "
                                      "'Deleted permanently!' -- release_box_all's "
                                      "own string (pdna_box.c), reused verbatim, N "
                                      "matching this box's real occupied count")
    # 19 individual gbs_delete() calls (top-down, one per occupied slot) take real
    # frames to settle -- measured by hand: BIG_SETTLE (40) is nowhere near enough
    # (the confirm dialog is still on screen), 700 reliably reaches gb_persist's wall.
    s.tap("A", settle=700)                                  # confirm -> delete top-down -> gb_persist
    s.shot("24_release_all_delta_wall", "BACKLOG #93: every slot deletes top-down "
                                         "(RAM-only), THEN gb_persist('release-all') "
                                         "hits the same PDNA_DELTA wall -- 'Edits are "
                                         "in-session only in the emulator build.'")
    s.tap("A", settle=250)                                  # dismiss -> box grid, no second dialog
    s.shot("25_grid_after_release_all", "BACKLOG #93: straight to the box grid after "
                                         "ONE dismiss -- BOX1 0/20, every deletion "
                                         "landed in g_ed->img despite the persist "
                                         "refusal, exactly like shot 09's single-slot "
                                         "case")

    # Bank-cell absence (#120 S2's own bank_edge UP hop, reused verbatim from
    # run_s2_bank -- 3 UPs from a fresh grid entry: cell -> title -> tabs -> the hop).
    s2 = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b93_{which}_bank_")
    s2.run(700)                      # #279: lands on the grid by itself (no info page, no tap)
    s2.run(GB_ART_COLD_SETTLE)
    s2.press_n("UP", 3, settle=100)
    s2.shot("26_bank_hop", "BACKLOG #93: the bank_edge UP hop opens the Bank -- "
                            "'BANK 1  7/30' -- the seven PDNA_DELTA-planted native "
                            "cells (five in slots 0-4, plus BACKLOG #150 S150-12 "
                            "decision 17's two COPY cells in slots 5/6), the rest "
                            "empty (#120 S2's F1 fix: no write surface survives "
                            "into a GB session's Bank visit, so nothing can ever "
                            "land here in mGBA)")
    s2.tap("A", settle=150)
    # D9 (review-opus, BACKLOG #93): this shot shows an EMPTY Bank cell -- there is
    # no occupied one to press A on in mGBA (#120 S2's F1 fix already closed the
    # only way anything lands in a GB session's Bank visit), so the proof this shot
    # backs is STRUCTURAL, not a screenshot of the actual row list being absent from
    # an occupied cell's popup: gb_bank_visit() calls app_src_readonly_clear()
    # BEFORE pdna_bank_show() (pdna_gen12.c), so g_src_ro is false for the entire
    # Bank visit and app_mon_menu_readonly -- where DUPLICATE/TO DAY-CARE/EXPORT all
    # live -- is never entered from inside the Bank at all, occupied cell or not.
    # Named/captioned to say exactly that, not to imply an occupied-cell test ran.
    s2.shot("27_bank_empty_cell_structural_proof",
            "BACKLOG #93 (D9): this Bank cell is EMPTY, not occupied -- mGBA has no "
            "way to get an occupied one here (#120 S2's F1 fix). A on it shows NO "
            "popup (silent snd_deny(), app_mon_menu's n == 0 branch on an empty "
            "cell), which is NOT itself a demonstration that DUPLICATE/TO DAY-CARE/"
            "EXPORT are absent from an OCCUPIED Bank cell's popup. The real proof is "
            "structural, not this screenshot: gb_bank_visit() calls "
            "app_src_readonly_clear() BEFORE pdna_bank_show() (pdna_gen12.c), so "
            "g_src_ro is false for the whole Bank visit and app_mon_menu_readonly -- "
            "where all three new rows live -- can never be entered from inside the "
            "Bank, occupied cell or not. An occupied Bank cell is itself "
            "HARDWARE-ONLY (a real Omega DE Bank keeps its contents across "
            "sessions) -- HW-QUEUE GBMON-6.", allow_same=True)
    s.taken += s2.taken
    s.skipped += s2.skipped
    return s


def run_gbnames(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """gbnames brief: real Gen-1/Gen-2 item names, now shown by the shipped GB-shell
    screens (pdna_gbbag.c's g1bag_paint_list, pdna_gbpack_body.inc's g2pack_paint_list)
    via gb_item_label() (source/gb_item_names.c, an embedded identifier table --
    GREEN per docs/kb/licensing.md, no ROM read, no locator). `which` picks the
    generation, same shape as run_u4_bag()/run_u5_pack() above:

      which="red":     `rom` MUST be a ONE-Gen-1-ROM fused image WITH Emerald.sav also
                        fused (tools/fuse_sav.py then tools/fuse_gb.py Red.gb+Red.sav --
                        BACKLOG #98's fused-image-by-generation harness gap, same
                        constraint run_u4_bag() documents; run_u4_bag()'s own fixture
                        convention, Emerald present). Nav is IDENTICAL to run_u4_bag()'s
                        own red path: boot picker DOWN -> A (box grid, #279: no info page) ->
                        START -> nav menu -> DOWN x7 (Party->Bank->Daycare->Trainer->
                        Clock fix->Mirage->Pokedex->Bag, PDNA_NAV_ITEMS index 7) -> A ->
                        pdna_gbbag_gen1_screen().
      which="crystal":  `rom` MUST be a ONE-Gen-2-ROM fused image WITHOUT Emerald.sav
                        (tools/fuse_gb.py straight off the base delta build, Crystal.gbc+
                        Crystal.sav ONLY -- run_u5_pack()'s own fixture convention, no
                        Emerald). Fusing Emerald.sav into this leg is a real footgun: the
                        boot picker then has 2 rows (Emerald first) and a single A lands
                        IN THE FUSED EMERALD SAVE'S OWN BAG (real Hoenn item names like
                        'WAILMER PAIL'/'DEVON SCOPE') instead of the Gen-2 Pack screen --
                        caught live capturing this slice's own shots, not a hypothetical.
                        A single-ROM-only image skips the boot picker (gb_delta_pick_
                        save()'s own `if (n == 1) return 0`, same as run_u5_pack()'s own
                        doc comment) -- nav is IDENTICAL to run_u5_pack()'s own crystal
                        path: A -> box grid (#279: no info page) -> START -> nav menu -> DOWN x7 ->
                        A -> pdna_gbpack_gen2_screen().

    Shots: the Items pocket (both gens show real names there by default -- id
    lookups, no ROM decode), Balls pocket on the Gen-2 leg only (the brief's own
    "Items + Balls pockets" ask), and one TM row per generation (both gens'
    TM/HM synthesis, HM/TM%02u, exercised live). The Gen-1 leg also scrolls one
    row past a long-vs-short name boundary (review-sonnet ask: prove the wider
    blank sweep leaves no stale glyph) -- this corpus's Red.sav Items pocket
    happens to open on its TMs (real save data, pickup order, not sorted by
    this core), so shot 01 already doubles as the "one TM row" ask; no
    ADD ITEM detour needed."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    if which == "red":
        s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "gbnames_red_")
        print("== gbnames: Red's own Item bag, real names ==")
        boot_to_gb_session(s, rom, which="red")
        s.tap("START", settle=gb_shots.BIG_SETTLE)          # box grid -> nav menu
        s.press_n("DOWN", 7)                                 # Party -> ... -> Bag (index 7)
        s.tap("A", settle=GB_ART_COLD_SETTLE)               # Bag -> pdna_gbbag_gen1_screen()
        s.shot("01_items_top_tm_rows", "gbnames: Red's Item bag, ITEMS pocket, top of "
                                        "the list -- this corpus save's Items pocket "
                                        "opens on TM05/TM06/TM27/TM29 (real save data, "
                                        "pickup order), all via gb1_tmhm_label() -- the "
                                        "brief's own 'one TM row' ask, not 'ITEM-n'")

        # Scroll to the real (tabled, non-TM/HM) names further down this same pocket --
        # DOWN x10 lands on TOWN MAP/BICYCLE/GOOD ROD/SUPER ROD* (8/7/8/9 chars, a real
        # mix of widths), one more DOWN scrolls the whole window by one row.
        s.press_n("DOWN", 10, settle=gb_shots.SETTLE)
        s.shot("02_scroll_before", "gbnames: DOWN x10 -- real (tabled) Gen-1 names "
                                    "now, not synthesized ones: TOWN MAP / BICYCLE / "
                                    "GOOD ROD / SUPER ROD* (8/7/8/9 chars) -- the "
                                    "'before' half of the no-stale-glyph scroll pair")
        s.tap("DOWN", settle=gb_shots.SETTLE)
        s.shot("03_scroll_after", "gbnames: one more DOWN -- the window shifts one "
                                   "row (TOWN MAP scrolls off, ITEMFINDER (10 chars) "
                                   "scrolls in at the bottom): BICYCLE / GOOD ROD / "
                                   "SUPER ROD* / ITEMFINDER -- every row shows exactly "
                                   "its own name with no leftover glyph from the row "
                                   "that used to be there (the widened NAME-row blank "
                                   "sweep, pdna_gbbag.c `cx < BOX_X1`)")

        # gbnames review A3 (CONFIRMED, fixed): a scroll pair over the SAME screen
        # row that specifically exercises the byte-vs-glyph fix -- POKe FLUTE (10
        # BYTES, but 9 GLYPHS: the e-acute's UTF-8 pair is one glyph) scrolling off,
        # replaced by REVIVE (6 chars, no multi-byte glyph at all) in that exact
        # row. Pre-fix, the sweep started at strlen("POKe FLUTE")=10 (one column
        # PAST where the name's own 9th glyph actually painted), leaving whatever
        # sat past column NAME_COL+10 unblanked -- the SAME defect class the
        # 'POKe BALLE' shot showed on the Gen-2 leg (gbnames_crystal_02).
        s.press_n("DOWN", 7, settle=gb_shots.SETTLE)        # total DOWN x18 from bag entry
        s.shot("04_poke_flute_before", "gbnames: DOWN x18 from bag entry -- POKe "
                                        "FLUTE / REVIVE / FULL RESTORE* / CANCEL "
                                        "(POKe FLUTE: 10 bytes, 9 glyphs) -- the "
                                        "'before' half of the glyph-specific scroll "
                                        "pair (A3)")
        s.tap("DOWN", settle=gb_shots.SETTLE)               # total DOWN x19 -- POKe FLUTE scrolls off
        s.shot("05_poke_flute_after", "gbnames: one more DOWN -- POKe FLUTE has "
                                       "scrolled off the top; REVIVE (6 chars, no "
                                       "multi-byte glyph) now sits in the EXACT "
                                       "screen row POKe FLUTE used to occupy, with "
                                       "no stray glyph left over (the fix: "
                                       "gbscr_text_cols(), glyph-accurate via "
                                       "gb_char_encode(), not strlen())")
        return s

    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "gbnames_crystal_")
    print("== gbnames: Crystal's own Pack, real names ==")
    boot_to_gb_session(s, rom, which="crystal")   # single-ROM image, no boot picker
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 7)                                     # Party -> ... -> Bag (index 7)
    s.tap("A", settle=GB_ART_COLD_SETTLE)                   # Bag -> pdna_gbpack_gen2_screen()
    s.shot("01_items_real_names", "gbnames: Crystal's Pack, ITEMS pocket -- real "
                                   "names from the embedded Gen-2 table, NOT "
                                   "'ITEM-n'; the NAME-row blank sweep now covers "
                                   "the full screen width (cols 8-19), not just "
                                   "the old QTY_COL=17 bound")

    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Items -> Balls
    s.shot("02_balls_real_names", "gbnames: RIGHT -> BALLS pocket -- 'POKe BALL' "
                                   "reads clean (review A3 fix: the NAME-row blank "
                                   "sweep now starts from gbscr_text_cols(), which "
                                   "counts GLYPHS via gb_char_encode(), not "
                                   "strlen()'s BYTE count -- the pre-fix version of "
                                   "this exact shot read 'POKe BALLE', a stray "
                                   "trailing 'E' left over from ULTRA BALL because "
                                   "strlen('POKe BALL')=10 overcounts the e-acute's "
                                   "2-byte UTF-8 pair as 2 glyphs instead of 1, "
                                   "starting the sweep one column short) -- the "
                                   "brief's own 'Items + Balls' ask")

    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Balls -> Key items
    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Key items -> TM/HM
    s.shot("03_tm_row", "gbnames: RIGHT x2 -> TM/HM -- the two-digit TM/HM number "
                         "prefix (cols 5-6) plus the SAME 'TM%02u'/'HM%02u' label "
                         "shape as the Gen-1 leg, this time driven by the flag-index "
                         "arithmetic g2pack_row_label()'s own TM/HM branch already "
                         "had (unaffected by this slice's id-based table -- that "
                         "branch never used a raw item id to begin with)")
    return s


def run_s2_control(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """#143: the Gen-3 CONTROL for the Bank menu -- what an Emerald Bank cell offers
    when accessed from a Gen-3 session (TO GAME / PASTE HERE / CREATE). `rom` MUST be
    `tools/fuse_sav.py <pokedna-delta-artless.gba> <Emerald.sav>` (a plain Gen-3
    fusion, no --gb, no --clip -- same vehicle as run_s2_bank_control), so the boot
    takes the Gen-3 path and lands straight on the party/box view.

    TAKE 2 (BACKLOG #143 reviewer findings, the two-BOUNCE retire): the previous cut
    of this function opened the Bank (source/pdna_bank.c) straight onto its box 0 and
    A'd whatever cell the cursor landed on first -- box 0's slots 0-4 are PLANTED
    native "GBC1" cells (PDNA_DELTA-only, source/bank_plant.c's bank_plant_box0(),
    called from pdna_bank.c:126 whenever box 0's real file can't be read, which is
    every fresh SD state on this fixture), so that always hit a Gen-1/2 cell and the
    app correctly routed it through app_mon_menu_readonly()'s native whitelist (VIEW
    only) -- a real behaviour, just the WRONG control (a native cell, not a Gen-3 one;
    the brief calls this out explicitly: "a native cell is not [an acceptable
    control]"). bank_plant.c plants Gen-3 records nowhere -- but as of BACKLOG #150
    S150-12 decision 17 it plants TWO MORE native cells (COPY cells) at slots 5/6, so
    slots 0-6 are ALL native now, not just 0-4; slots 7-29 of box 0 are the first
    genuinely empty ones (ordinary zeroed Gen-3 box slots) -- so the only way to see
    an OCCUPIED Gen-3 Bank cell's menu is to put a real Gen-3 record there first: COPY
    a mon off the save's own box (box 0 of the SAVE's PC, not the Bank -- app_mon_
    menu's ordinary occupied-cell menu, reached straight off the boot cursor) into the
    clipboard, then PASTE HERE into an empty (non-native) Bank slot. This run does
    exactly that, entirely through taps a player has -- no ROM/save file is edited
    directly.

    Bank grid layout confirmed live (source/pdna_bank.c's BOX_RECS=30, pdna_box.c's
    6-column grid, COLS=6): slot 6 is row 1, column 0 (one DOWN from slot 0) -- now
    OCCUPIED by decision 17's second planted COPY cell (found live while re-verifying
    this chain after decision 17 landed: the original "DOWN then A" recipe silently
    opened that native cell's own whitelist menu instead of CREATE/PASTE HERE, and
    the two unrelated taps that followed cascaded into leaving the Bank entirely and
    editing a PARTY mon's menu -- not this lane's own gate, a stale navigation count
    in this test script, fixed here). Slot 7 is row 1, column 1 (one more RIGHT) --
    the first genuinely empty Gen-3 slot; slot 8 (row 1, column 2, one more RIGHT
    again) is the second.

    Unlike run_s2_bank() which probes the Bank from a GB session (showing why F1 closed
    it off: no CREATE, no PASTE HERE on an empty cell), this run shows the Gen-3
    Bank's own menu structure: an occupied cell offers TO GAME (send to the game's own
    party/PC), and an empty cell offers CREATE and PASTE HERE (import from clipboard
    or create a new Gen-3 mon in the Bank). The F1 gate (xg_create_row/xg_paste_row)
    does NOT apply here -- only to the GB session's Bank visit; a Gen-3 session's Bank
    visit always shows the full menu."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s2_control_")
    print("== #143: the Gen-3 Bank control -- what CREATE + PASTE HERE look like ==")
    s.run(700)
    s.shot("00_boot", "#143: Emerald boots straight into the party/box view, cursor "
           "on box 0 slot 0 -- No.81 TRIEYE (the mon's own NICKNAME from the save; the species line reads MAGNEMITE) "
           "Lv24, a real Gen-3 record in the SAVE's own PC (not the Bank)")

    # Copy the boot cursor's own Gen-3 mon off the save's PC into the clipboard --
    # app_mon_menu's ORDINARY occupied-cell menu (is_bank == false): VIEW/EDIT, ITEM,
    # LEGALITY, MOVE, COPY, DUPLICATE, TO DAY-CARE, EXPORT .pk, RELEASE (live-traced;
    # COPY is row index 4). Confirmed live: this is the SAVE's PC box, not the Bank
    # (the row reads "MOVE", not "MOVE TO BOX" -- is_party is false here).
    s.tap("A", settle=150)                                     # box cell 0 -> its menu
    s.press_n("DOWN", 4, settle=gb_shots.SETTLE)               # -> COPY row
    s.tap("A", settle=150)                                     # COPY -> "COPIED" dialog
    s.tap("A", settle=100)                                     # dismiss -> back to box view

    s.tap("START", settle=80)                                  # nav menu
    s.tap("DOWN", settle=60)                                   # Party -> Bank (index 1, one DOWN)
    s.tap("A", settle=150)                                     # -> pdna_bank_show(), box 0 (BANK 1)
    s.shot("01_bank_grid", "#143: the Bank, from a Gen-3 session -- BANK 1 (box 0) "
           "shows the PDNA_DELTA plant's 7 native cells (CHI1/PIK/EGG/CHI2/DMG at "
           "slots 0-4, bank_plant_box0, plus BACKLOG #150 S150-12 decision 17's two "
           "COPY cells at slots 5/6) across its top row and genuinely empty Gen-3 "
           "slots everywhere else (7/30 occupied); cursor on slot 0 (CHI1, native)")

    s.tap("DOWN", settle=gb_shots.SETTLE)                      # slot 0 -> slot 6 (row 1, col 0): NATIVE (decision 17's copy cell)
    s.tap("RIGHT", settle=gb_shots.SETTLE)                     # slot 6 -> slot 7 (row 1, col 1): empty, non-native
    s.tap("A", settle=150)                                     # A on the empty Gen-3 slot
    s.tap("DOWN", settle=gb_shots.SETTLE)                      # CREATE -> PASTE HERE
    s.tap("A", settle=150)                                     # PASTE HERE -> commits the clipboard mon into slot 7

    s.tap("A", settle=150)                                     # A again on the now-OCCUPIED slot 7
    s.shot("02_occupied_cell_menu", "#143: an OCCUPIED Gen-3 Bank cell's menu (slot "
           "7, just pasted from the save's own PC -- slots 5/6 are decision 17's own "
           "planted COPY cells now, so this chain targets the first genuinely empty "
           "slot instead) from a Gen-3 session -- VIEW/EDIT, ITEM, LEGALITY, MOVE, "
           "COPY, PASTE, DUPLICATE, TO GAME, RELEASE -- TO GAME is the Gen-3-only row "
           "(send the mon into the loaded save), present because app_mon_menu (not "
           "the read-only variant) is driving this cell")

    s.tap("B", settle=100)                                     # back to the grid
    s.tap("RIGHT", settle=gb_shots.SETTLE)                     # slot 7 -> slot 8 (row 1, col 2): still empty
    s.tap("A", settle=150)                                     # A on the empty Gen-3 slot
    s.shot("03_empty_cell_menu", "#143: an EMPTY Gen-3 Bank cell's menu (slot 8, "
           "distinct from the slot the previous shot just filled) from a Gen-3 "
           "session -- CREATE / PASTE HERE / CANCEL -- xg_create_row and xg_paste_row "
           "are both true in an ordinary Gen-3 session (pc_live is true, parsed save, "
           "arena free, clipboard still holds the Magnemite copy from the boot step), "
           "so both rows present on the empty cell (TO GAME is absent because the "
           "cell is empty, not because of a gate)")
    return s


def run_b187_chain_a(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #187/#192, review fix 2, Chain A: SELECT toggles NORMAL -> MOVE ->
    NORMAL on an EMPTY cell (box 12, the current box, boots 0/20 -- current-box
    byte 0x284C=0x8B on Guy's own Yellow.sav).

    Before F1 (source/pdna_box.c's SELECT dispatch), ENTERING MOVE mode required
    an OCCUPIED cell (gb_can_lift_hook_impl -> gbs_can_delete refuses
    slot >= count on an empty one), and LEAVING it used the exact SAME gate -- so
    moving the cursor onto an empty cell while already in MOVE, then pressing
    SELECT again, hit that same refusal and did nothing: #192, "stuck on orange
    (grab)". Proven live against the PRE-fix build (this lane's own repro run):
    a second SELECT produced a pixel-identical frame to the one before it (this
    file's own identical-frame shot guard raised a RuntimeError).

    F1 makes leaving any non-NORMAL mode unconditional, and makes ENTERING
    box-level (BoxSource.can_enter_move, gbsrc_can_enter_move =
    app_can_edit() && gbs_box_writable(box)) instead of per-cell -- so this
    chain now demonstrates something that was IMPOSSIBLE before the fix at all:
    entering MOVE from an empty cell in the first place, not just leaving it.

    `rom` MUST be the flight-shaped image (Emerald.sav + a GB corpus fused with
    tools/fuse_gb.py) with Guy's OWN Yellow.sav as its Gen-1 payload (copy it to
    /tmp first -- the corpus at gba-toolkit/roms/gb/Yellow.sav is read-only)."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b187a_")
    print("== BACKLOG #187/#192, Chain A: SELECT NORMAL<->MOVE on an empty cell ==")
    boot_to_gb_session(s, rom, which="yellow")
    s.shot("00_box12_normal", "tap0 (boot): box12 (current box, 0/20), cell(0,0) "
           "empty, NORMAL mode -- footer 'A menu SEL L/R B'",
           claim=["A menu  SEL  L/R  B"])   # BACKLOG #214 item 3 retrofit
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)
    s.shot("01_select_to_move", "tap1 (SELECT): entering MOVE is now box-level "
           "(gbsrc_can_enter_move), not per-cell -- footer changes to 'MOVE A "
           "grab hold=set', cursor icon changes -- IMPOSSIBLE pre-fix on an "
           "empty cell (gbs_can_delete would have refused)",
           claim=["MOVE A grab hold=set"])   # BACKLOG #214 item 3 retrofit
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)
    s.shot("02_select_back_to_normal", "tap2 (SELECT): leaving is now "
           "unconditional -- footer back to 'A menu SEL L/R B' -- pre-fix, this "
           "EXACT second SELECT on an empty cell was #192's stuck case (proven "
           "live: pixel-identical frame before/after on the pre-fix build)",
           claim=["A menu  SEL  L/R  B"])   # BACKLOG #214 item 3 retrofit
    return s


def run_b187_chain_b(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #187/#193, review fix 2, Chain B: DUPLICATE on box 1 (genuinely
    20/20 full on Guy's own Yellow.sav) opens F3's destination picker instead of
    refusing flat, defaults to BOX8 (11/20, the first box with room), and lands
    there (box 8's count visibly goes 11/20 -> 12/20).

    Navigation notes (re-derived by incremental probing against this exact
    image, not assumed -- this file's own module docstring already documents
    the d-pad auto-repeat gotcha that makes a fixed L/R press count unreliable
    across runs): 17x L from the box-12 boot landing reaches box 1 on this
    build (verified: box 12 -1L-> box 11 -1L-> box 10 ... continuing past box 1
    wraps to the party, so 17 is the count that lands ON box 1, not past it);
    7x R from box 1, after the DUPLICATE flow below, reaches box 8. A generous
    settle (60 frames) after every L/R tap plus a trailing 60-frame idle run
    avoids the missed-input failure mode a shorter settle hit during this
    lane's own repro work.

    `rom` MUST be the same flight-shaped Yellow.sav image run_b187_chain_a()
    uses."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b187b_")
    print("== BACKLOG #187/#193, Chain B: DUPLICATE box1(full) -> picker -> box8 ==")
    boot_to_gb_session(s, rom, which="yellow")
    for _ in range(17):
        s.tap("L", settle=60)
    s.run(60)
    s.shot("00_box1_grid", "tap0 (17xL, settled): box1 grid -- '1:GB BOX1 20/20'")
    s.tap("A", settle=100)
    s.shot("01_mon_menu", "tap1 (A on cell(0,0), Bulbasaur): mon menu opens, "
           "row0 VIEW/EDIT selected")
    s.press_n("DOWN", 4, settle=30)
    s.shot("02_dup_selected", "tap2 (DOWN x4): cursor on DUPLICATE (Gen-1 row "
           "order: VIEW/EDIT, LEGALITY, MOVE TO BOX, COPY, DUPLICATE)")
    s.tap("A", settle=100)
    s.shot("03_picker_opens", "tap3 (A on DUPLICATE): F3 -- box1 is 20/20 full, "
           "so the 'DUPLICATE TO' picker opens instead of a flat refusal; BOX1-7 "
           "all show 20/20 and are dimmed (full, per F3's own gb_pick_box "
           "change), BOX8 11/20 is pre-selected (the first selectable box)")
    s.tap("A", settle=150)
    s.shot("04_delta_wall", "tap4 (A picks BOX8): the PDNA_DELTA honest wall "
           "('Edits are in-session only in the emulator build.') -- hardware-"
           "only for the actual persisted confirmation ('Landed in slot N'), "
           "same posture every other GB write path hits under mGBA (no SD "
           "card in the emulator)")
    s.tap("A", settle=150)
    for _ in range(7):
        s.tap("R", settle=60)
        s.run(40)
    s.shot("05_box8_after", "tap5 (7xR from box1, settled): box8 grid -- "
           "'8:GB BOX8 12/20' -- count went 11/20 -> 12/20, the duplicate "
           "landed there (gb_persist's PDNA_DELTA branch re-baselines "
           "g_ed->img to the post-edit bytes even though the CARD write is "
           "refused -- the in-session image IS updated, same posture every "
           "other GB write path takes under mGBA)")
    return s


def run_b187_chain_c(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #187, review fix 2, Chain C: CREATE in box 12 (the current box,
    0/20, real room) opens the species picker -- the unmodified, already-working
    path (F4 only changes the FULL-box case; this chain demonstrates F4 did not
    regress the ordinary one). `rom` MUST be the same flight-shaped Yellow.sav
    image run_b187_chain_a() uses."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b187c_")
    print("== BACKLOG #187, Chain C: CREATE in box12 (current, 0/20) ==")
    boot_to_gb_session(s, rom, which="yellow")
    s.shot("00_box12_boot", "tap0 (boot): box12 (current box, 0/20)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("01_empty_menu", "tap1 (A on cell(0,0), empty): EMPTY/CREATE/CANCEL "
           "menu")
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("02_species_picker", "tap2 (A on CREATE): species picker opens, "
           "No.001 BULBASAUR pre-selected -- F4's own full-box retry path never "
           "triggers here (box12 has real room)")
    return s


# BACKLOG #187/#193/#191a/#192, review fix 2 said chain D (drag-and-drop, SELECT ->
# MOVE -> A to lift -> L/R to another box -> A to drop) was impossible on ANY
# emulator vehicle: the lift half of a GB-scope carry (start_carry -> BoxXferOps.
# lift_up -> gb_lift_up_hook) ALWAYS called pdna_bank_next_serial() -> meta_save() ->
# a REAL SD write, which PDNA_DELTA (no SD card) always refused before a single cell
# moved -- ANY drag, not just a cross-scope one. BACKLOG #199 (lane b199) moved that
# call from the grab (start_carry) to the ONE drop that actually needs it
# (drop_held_up, the GB -> Bank UP drop) -- a within-save drag now never calls
# lift_up at all, and is genuinely shootable for the first time. run_b199_chain_d()
# below is that proof: D1 is the within-save move (asks nothing, writes nothing);
# D2 is the GB -> Bank drop (asks + writes, --vsd attached so the write is real).


def run_b199_chain_d(core_mod, image_mod, rom: Path, out_dir: Path) -> list[gb_shots.Session]:
    """BACKLOG #199 (lane b199): chain D, the drag-and-drop gesture BACKLOG #187's
    review fix 2 could never shoot before this lane (see the module comment just
    above). Two sub-chains, each its own Session, each attaching --vsd (BACKLOG
    #179's virtual SD) so a real write can be told apart from a refused one.

    `rom` MUST be a flight-shaped image (Emerald.sav + a fused Yellow.gb/Yellow.sav,
    tools/fuse_sav.py then tools/fuse_gb.py) with Guy's OWN Yellow.sav as the Gen-1
    payload (copy it to /tmp first -- the corpus at gba-toolkit/roms/gb/Yellow.sav is
    read-only) -- the SAME image run_b187_chain_a/b/c() use. Navigation re-derived by
    direct probing against this exact image (not assumed): box1 (index 0, 20/20
    full, run_b187_chain_b's own corpus fact) is reached from the box12 boot landing
    (index 11) by 2x R (11->12 the GB PARTY pseudo-box->0 box1, SWITCH_BOX's
    `(box+1)%nb` wrap, nb=13) -- confirmed live; a single R plus a shot lands on the
    PARTY pseudo-box, not box1, so the run needs an idle settle run between taps
    (60 frames) or the second R's own edge is served mid-paint of the first.

    D1 -- WITHIN-SAVE MOVE (box1 slot 0 -> box12, both boxes of the SAME save):
    SELECT (MOVE mode) -> A (lift slot 0) -> two carry-mode L presses (SWITCH_BOX
    wraps 0->12 the party pseudo-box->11 box12, the empty boot-landing box) -> A
    (drop on the empty cell). Verified live: step 02 (the lift) shows NO origin
    prompt at all -- before this lane this EXACT step opened "WHICH GAME IS THIS?"
    full-screen (see run_s150_4_uplift's own captured frame 02, still true for any
    GB->Bank carry, just not this one any more) -- start_carry() now only memcpy's
    the display record. The drop reaches gb_move_core/gbs_move for real and hits the
    SAME honest PDNA_DELTA wall ("Edits are in-session only in the emulator build.")
    every other GB write in this file already hits (the FUSED GB save itself has no
    real SD path under delta, independent of the Bank's own --vsd) -- s_holding stays
    true across that wall (drop_held's `if (!ok) return recs;`, ok = gb_persist()'s
    own false), same shape as Chain B's DUPLICATE-then-wall. --vsd's own report is
    the proof that matters: ["/PokeDNA/log.txt"] only (the triple logger's own
    flush from inside gb_persist's refusal branch) -- no bank.meta, no ledger file,
    nothing under /PokeDNA/xfer/ or /PokeDNA/bank/ at all. THAT is "asks nothing,
    writes no serial", mechanically, not just by absence of a screenshot.

    D2 -- GB -> BANK DROP (box1 slot 0 -> the Bank, cross-scope): SELECT -> A (lift,
    also no prompt) -> two carry-mode UP presses (row0 -> tab focus -> the bank_edge
    hop, `return 4`) lands on the Bank, STILL CARRYING -- and the carried icon is
    the REAL Bulbasaur sprite riding the glove, not a badge-only fist (b199's own
    consequence: s_held is the Gen-3-shaped display record the WHOLE hold, so
    oam_sync's `pk_decode_mon(s_held, ...)` branch runs instead of the
    bc_is_native() badge branch -- before this lane, a GB-origin carry could never
    even reach here to compare). A on an empty Bank cell triggers drop_held_up,
    which now calls `s_xfer_peer->lift_up(s_orig_box, s_orig_slot, packed)` BY
    ORIGIN COORDINATES for the first time (this exact call was impossible to reach
    live before b199 -- the grab always refused first). Found live, not assumed:
    box1 slot 0 (this exact corpus mon) already carries a bank_plant.c dev fixture
    (`bank_plant_site2_seed`, xfer_io.c's own `xr_open()` fallback to
    `bank_plant_xfer_open()` when the real SD read misses) -- so the lift takes the
    RESTORE branch (gb_lift_restore, "BACK TO ITS ORIGINAL", Level 5 > 10 / Nickname
    changed) rather than the plain fresh-mon origin picker; the ASKS+WRITES contract
    the brief names is still proven, by the SAME two real facts (a screen the player
    must act on, then pdna_bank_next_serial()'s own verified SD write) -- arguably a
    STRONGER proof, since it exercises the shared merge-screen/serial pipeline
    end-to-end rather than the simpler fresh-mon path. A flips the level row to TAKE,
    START applies: --vsd shows the REAL writes landing -- bank.meta (+ .bak),
    backup-v1/bank.meta + DONE (pdna_bank_prepare_native()'s first-ever-run backup
    gate), box00.box (the Bank cell itself), and log.txt. The GB-side release then
    hits the SAME PDNA_DELTA wall D1's drop did (the fused save has no SD path under
    delta) -- release_up returns false, and drop_held_up's own KEPT message shows
    ("MOVED TO THE BANK / It is still in the Game Boy save too - remove it there.")
    -- a duplicate, not a loss, exactly the ordering the design promises (S150-4's
    serial-before-cell / cell-before-delete, unchanged by this lane's move).

    b199 review D4/R1 (BACKLOG #199 review, docs/briefs/b199-review.md): R1's own
    stop-licence condition -- a carried Pokemon must never be able to exist in
    NEITHER place at an instant a power cut could freeze -- gets its own SWEPT
    sub-chain, D3 below, armed at the exact moment the Bank commit begins
    (fail_at/lie_after set on a FRESH VsdImage per swept value, isolated from
    D1/D2's own --vsd image and from any --vsd-fail-at/--vsd-lie-after the CLI
    parsed for this whole process). For every value: still holding (footer 'A
    drop  B cancel'), never 'nowhere' -- the answer now lives in this repo, not
    only in a review transcript."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    sessions: list[gb_shots.Session] = []

    # BACKLOG #199 review D4 (repair): set_default_vsd(img) with NO knobs OVERWRITES
    # gb_shots._DEFAULT_VSD_KNOBS wholesale (it is a plain module global, replaced not
    # merged) -- so any --vsd-fail-at/--vsd-lie-after/--vsd-protect the CLI already
    # parsed into it (dgb_shots.main()'s own gb_shots.set_default_vsd(a.vsd, ...) call,
    # BEFORE this function ever runs) was silently discarded the moment either D1's or
    # D2's own set_default_vsd(vsd1)/(vsd2) call below ran. Proven: --vsd-fail-at 1
    # against this exact chain gave a byte-identical result set to no flag at all.
    # Captured ONCE, here, before either call below can clobber it, and forwarded to
    # both -- a caller who armed a chain-wide injection knob on the command line now
    # actually gets it applied to D1 and D2's own images too.
    _cli_vsd_knobs = dict(gb_shots._DEFAULT_VSD_KNOBS)

    # ---- D1: within-save move, box1 slot0 -> box12 (empty) ---------------------
    vsd1 = out_dir / "b199d1.img"
    binp = gb_shots._vsd_img_bin()
    if vsd1.exists():
        vsd1.unlink()
    r = subprocess.run([str(binp), "mkimg", str(vsd1), "16"], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"b199 chain D1: mkimg failed: {r.stderr.strip()}")
    gb_shots.set_default_vsd(vsd1, **_cli_vsd_knobs)

    s1 = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b199d1_")
    print("== BACKLOG #199 Chain D1: within-save GB move (--vsd) ==")
    boot_to_gb_session(s1, rom, which="yellow")
    s1.vsd_snapshot()   # reset the diff baseline AFTER boot's own log.txt/MIGRATED writes
    s1.tap("R", settle=gb_shots.BIG_SETTLE); s1.run(60)
    s1.tap("R", settle=gb_shots.BIG_SETTLE); s1.run(60)
    s1.shot("00_box1", "tap0 (2xR from boot): box1 (index 0, 20/20 full) -- "
            "run_b187_chain_b's own corpus fact, reached via the party pseudo-box wrap")
    s1.tap("SEL", settle=gb_shots.BIG_SETTLE); s1.run(60)
    s1.shot("01_move_mode", "tap1 (SELECT): MOVE mode -- footer 'MOVE A grab hold=set'",
            claim=["MOVE A grab hold=set"])
    s1.tap("A", settle=gb_shots.BIG_SETTLE); s1.run(60)
    s1.shot("02_lifted_no_prompt", "tap2 (A, lift slot 0): carrying, footer 'A drop "
            "B cancel' -- NO origin prompt (BACKLOG #199's whole point: start_carry() "
            "no longer calls lift_up() for ANY scope; the grab is a plain memcpy of "
            "the display record). Before this lane the identical A press here opened "
            "a full-screen 'WHICH GAME IS THIS?' picker (run_s150_4_uplift frame 02) "
            "and only then refused on the serial write -- this frame proves that no "
            "longer happens at grab time.",
            claim=["A drop  B cancel"])
    s1.tap("L", settle=gb_shots.BIG_SETTLE); s1.run(60)
    s1.tap("L", settle=gb_shots.BIG_SETTLE); s1.run(60)
    s1.shot("03_box12_empty", "tap3 (2xL, carrying): box12 (index 11, 0/20, the boot "
            "landing) -- SWITCH_BOX wraps 0 -> 12 (party) -> 11 while still carrying, "
            "still no card write of any kind")
    s1.tap("A", settle=gb_shots.BIG_SETTLE); s1.run(100)
    s1.shot("04_delta_wall", "tap4 (A, drop on the empty cell): move_within reaches "
            "gb_move_core/gbs_move for real, then hits gb_persist()'s own honest "
            "PDNA_DELTA wall ('Edits are in-session only in the emulator build.') -- "
            "the SAME wall every other GB write in this file shows on this vehicle "
            "(no real SD for the FUSED save under delta, independent of the Bank's "
            "own --vsd); s_holding stays true (drop_held's `if (!ok) return recs;`)",
            claim=["GAME BOY SAVE", "Edits are in-session only",
                   "in the emulator build."])

    # ---- RED DEMO first (BACKLOG #184: a proof that cannot fail is not a proof) --
    try:
        s1.vsd_report(expect_changed=["/PokeDNA/this/path/does/not/exist.pds"])
        raise RuntimeError("D1 RED DEMO FAILED: vsd_report() did not exit(1) on a "
                            "deliberately wrong expect_changed set")
    except SystemExit as e:
        if e.code != 1:
            raise RuntimeError(f"D1 RED DEMO: vsd_report() exited {e.code}, not 1")
        print("  [RED DEMO] D1 vsd_report(expect_changed=[wrong path]) correctly "
              "exited 1 -- the GREEN proof below is not inert")
    changed1 = s1.vsd_report(expect_changed=[
        "/PokeDNA/log.txt",   # the triple logger's own flush, from INSIDE
                              # gb_persist()'s PDNA_DELTA refusal branch -- the ONLY
                              # thing this whole gesture wrote anywhere
    ])
    print(f"  [VSD] D1 vsd_diff (GREEN, expected set matched): {sorted(changed1)} "
          f"-- no bank.meta, no ledger file, nothing under /PokeDNA/xfer or "
          f"/PokeDNA/bank at all: a within-save move asks nothing and writes no "
          f"serial, mechanically proven, not just absent from a screenshot")
    sessions.append(s1)

    # ---- D2: GB -> Bank drop, box1 slot0 -> the Bank (cross-scope) --------------
    vsd2 = out_dir / "b199d2.img"
    if vsd2.exists():
        vsd2.unlink()
    r = subprocess.run([str(binp), "mkimg", str(vsd2), "16"], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"b199 chain D2: mkimg failed: {r.stderr.strip()}")
    gb_shots.set_default_vsd(vsd2, **_cli_vsd_knobs)

    s2 = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b199d2_")
    print("== BACKLOG #199 Chain D2: GB -> Bank drop (--vsd) ==")
    boot_to_gb_session(s2, rom, which="yellow")
    s2.vsd_snapshot()
    s2.tap("R", settle=gb_shots.BIG_SETTLE); s2.run(60)
    s2.tap("R", settle=gb_shots.BIG_SETTLE); s2.run(60)
    s2.shot("00_box1", "tap0 (2xR from boot): box1 (20/20 full), same landing D1 uses")
    s2.tap("SEL", settle=gb_shots.BIG_SETTLE); s2.run(60)
    s2.tap("A", settle=gb_shots.BIG_SETTLE); s2.run(60)
    s2.shot("01_lifted_no_prompt", "tap1 (SELECT, A): lifted slot 0, no origin prompt "
            "(same b199 fact D1's tap2 shows)",
            claim=["A drop  B cancel"])
    s2.tap("UP", settle=gb_shots.BIG_SETTLE); s2.run(60)
    s2.tap("UP", settle=150); s2.run(100)
    s2.shot("02_bank_hop", "tap2 (2xUP, carrying): row0 -> tab focus -> the bank_edge "
            "hop (`return 4`) -- lands on the Bank, STILL carrying, and the carried "
            "icon is the REAL Bulbasaur sprite riding the glove (not a badge-only "
            "fist) -- b199's other visible consequence: s_held is the Gen-3-shaped "
            "display record for the WHOLE hold, so oam_sync's pk_decode_mon() branch "
            "runs here instead of the bc_is_native() native-badge branch; before "
            "this lane a GB-origin carry could never even reach the Bank to compare")
    s2.tap("A", settle=gb_shots.BIG_SETTLE); s2.run(100)
    s2.shot("03_merge_screen", "tap3 (A on an empty Bank cell): drop_held_up calls "
            "`s_xfer_peer->lift_up(s_orig_box, s_orig_slot, packed)` BY ORIGIN "
            "COORDINATES for the first time -- unreachable live before b199 (the "
            "grab always refused first). Found live, not assumed: this exact corpus "
            "mon (box1 slot 0) already carries a bank_plant.c dev fixture "
            "(bank_plant_site2_seed, xfer_io.c's xr_open() fallback to "
            "bank_plant_xfer_open() when the real SD read misses), so the lift takes "
            "the RESTORE branch -- 'BACK TO ITS ORIGINAL', 'Level 5 > 10' TAKE row, "
            "'Nickname changed' KEEP row -- rather than the plain fresh-mon origin "
            "picker. The brief's ASKS+WRITES contract is proven either way: this IS "
            "a screen the player must act on, and applying it still spends a real "
            "serial (below) -- the shared merge-screen/serial pipeline, exercised "
            "end to end.",
            claim=["BACK TO ITS ORIGINAL", "Level 5 > 10", "Nickname changed"])
    s2.tap("A", settle=gb_shots.BIG_SETTLE); s2.run(60)     # flip the level row to TAKE
    s2.shot("04_flipped", "tap4 (A): the level row flips to TAKE")
    s2.tap("START", settle=gb_shots.BIG_SETTLE); s2.run(150)
    s2.shot("05_delta_wall", "tap5 (START, apply): the Bank write lands FOR REAL on "
            "this --vsd image (see the vsd_diff below) -- then release_up's own "
            "gb_persist() call hits the SAME honest PDNA_DELTA wall D1's drop showed "
            "('Edits are in-session only in the emulator build.') -- the FUSED GB "
            "save has no real SD path under delta, independent of the Bank's own "
            "--vsd, so the ORIGIN half of this drop cannot be verified-deleted here",
            claim=["GAME BOY SAVE", "Edits are in-session only",
                   "in the emulator build."])
    s2.tap("A", settle=gb_shots.BIG_SETTLE); s2.run(150)
    s2.shot("06_kept_dup", "tap6 (A, dismiss): release_up returned false (the wall "
            "above), so drop_held_up shows its own KEPT message -- 'MOVED TO THE "
            "BANK / It is still in the Game Boy save too - remove it there.' -- a "
            "DUPLICATE, never a loss: the Bank cell landed and verified BEFORE the "
            "GB-side delete was even attempted (S150-4's own ordering, unmoved by "
            "this lane -- see drop_held_up's header comment)",
            claim=["MOVED TO THE BANK", "It is still in the Game Boy",
                   "save too - remove it there."])

    # ---- RED DEMO first ----------------------------------------------------------
    try:
        s2.vsd_report(expect_changed=["/PokeDNA/this/path/does/not/exist.pds"])
        raise RuntimeError("D2 RED DEMO FAILED: vsd_report() did not exit(1) on a "
                            "deliberately wrong expect_changed set")
    except SystemExit as e:
        if e.code != 1:
            raise RuntimeError(f"D2 RED DEMO: vsd_report() exited {e.code}, not 1")
        print("  [RED DEMO] D2 vsd_report(expect_changed=[wrong path]) correctly "
              "exited 1 -- the GREEN proof below is not inert")
    changed2 = s2.vsd_report(expect_changed=[
        "/PokeDNA/bank/backup-v1/DONE",         # pdna_bank_prepare_native()'s
                                                 # first-ever-run backup gate marker
        "/PokeDNA/bank/backup-v1/bank.meta",    # the backup-v1 copy of bank.meta
                                                 # bank_backup_v1() takes before the
                                                 # first-ever native write
        "/PokeDNA/bank/bank.meta",              # pdna_bank_next_serial()'s own
                                                 # verified meta_save() -- the ONE
                                                 # write the brief calls "a serial"
        "/PokeDNA/bank/bank.meta.bak",          # sf_write_verified's own rolling
                                                 # backup of bank.meta
        "/PokeDNA/bank/box00.box",              # the Bank cell itself, verified
                                                 # box_save() -- the "asks" half's
                                                 # answer actually landing
        "/PokeDNA/log.txt",                     # the triple logger's own append
    ])
    print(f"  [VSD] D2 vsd_diff (GREEN, expected set matched): {sorted(changed2)} "
          f"-- a real bank.meta write AND a real Bank-cell write: a GB -> Bank drop "
          f"asks (this screen) and writes (this serial), mechanically proven")
    sessions.append(s2)

    # ---- D3/R1 (b199 review D4): sweep a mid-drop Bank-write failure ------------
    # R1's own stop-licence condition: a carried Pokemon must never be able to exist
    # in NEITHER place at an instant a power cut could freeze. Re-runs D2's exact tap
    # sequence up to (and including) START on a FRESH Session/vsd image per swept
    # value, arming the injection knob DIRECTLY ON THE LIVE VsdImage (s3.vsd.image.
    # <knob> -- a plain mutable field, the same one write()/read() in tools/vsd.py
    # check) right before the START tap, so the failure is live for exactly this
    # commit and nothing before it. An earlier cut of this sweep passed the knob to
    # Session.__init__ instead (vsd_knobs=) -- found live to be WRONG: the knob is
    # then live from the very first frame, so it fires inside BOOT's own writes and
    # breaks boot_to_gb_session()'s own gb_info_page assertion before the chain ever
    # reaches box1 -- not "mid-drop" at all.
    #
    # Four points, chosen empirically against THIS exact commit (not guessed from the
    # write-set's byte sizes, which turned out not to predict sector-call order):
    # fail_at=1 and fail_at=8 both land inside the write sequence (confirmed by a
    # partial vsd_report() diff, fewer than the full 5-file set D2's own successful
    # run produces); fail_at=20 lands even later, in the Bank BACKUP step specifically
    # (a THIRD, distinct refusal dialog was found live here -- "COULD NOT PREPARE /
    # The Bank backup failed." -- neither of the other two dialogs this brief already
    # names); lie_after=0 is R1's sharper case (every write ACKs but DISCARDS, the
    # card that lies from the very first call) -- does sf_write_verified's own
    # read-back catch it, or does the code believe a lie and proceed as if committed?
    # Each Session is its OWN vsd_img (NOT gb_shots.set_default_vsd()), isolated from
    # D1/D2's own image and from _cli_vsd_knobs above -- this sweep's own injection
    # must never leak into D1/D2, nor theirs into this.
    _sweep_points = [("fail_at", 1), ("fail_at", 8), ("fail_at", 20), ("lie_after", 0)]
    for knob_name, knob_val in _sweep_points:
        vsd3 = out_dir / f"b199d3_{knob_name}_{knob_val}.img"
        if vsd3.exists():
            vsd3.unlink()
        r = subprocess.run([str(binp), "mkimg", str(vsd3), "16"], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"b199 chain D3 ({knob_name}={knob_val}): mkimg failed: {r.stderr.strip()}")
        s3 = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b199d3_{knob_name}{knob_val}_",
                               vsd_img=vsd3)   # clean boot -- no vsd_knobs, see above
        print(f"== BACKLOG #199 Chain D3/R1: mid-drop Bank-write sweep ({knob_name}={knob_val}) ==")
        boot_to_gb_session(s3, rom, which="yellow")
        s3.vsd_snapshot()   # reset AFTER boot's own log.txt/MIGRATED writes, same as D1/D2
        s3.tap("R", settle=gb_shots.BIG_SETTLE); s3.run(60)
        s3.tap("R", settle=gb_shots.BIG_SETTLE); s3.run(60)      # box1 (20/20 full)
        s3.tap("SEL", settle=gb_shots.BIG_SETTLE); s3.run(60)    # MOVE mode
        s3.tap("A", settle=gb_shots.BIG_SETTLE); s3.run(60)      # lift slot 0, no prompt
        s3.tap("UP", settle=gb_shots.BIG_SETTLE); s3.run(60)
        s3.tap("UP", settle=150); s3.run(100)                    # bank_edge hop, still carrying
        s3.tap("A", settle=gb_shots.BIG_SETTLE); s3.run(100)     # empty Bank cell -> merge screen
        s3.tap("A", settle=gb_shots.BIG_SETTLE); s3.run(60)      # flip level row to TAKE
        setattr(s3.vsd.image, knob_name, knob_val)                # ARM here, mid-drop, right
                                                                    # before the write-issuing tap
        s3.tap("START", settle=gb_shots.BIG_SETTLE); s3.run(150)  # apply -- the injected knob
                                                                   # fires somewhere inside THIS commit
        s3.shot(f"00_dialog_{knob_name}{knob_val}",
                f"D3/R1 ({knob_name}={knob_val}): the frame immediately after START, "
                f"with the injected failure armed for this exact commit -- whichever "
                f"of the codebase's refusal dialogs fires here (found live: 'NOT "
                f"MOVED TO THE BANK' for the plain lift_up() refusal, or 'COULD NOT "
                f"PREPARE / The Bank backup failed.' for a failure inside "
                f"pdna_bank_prepare_native() specifically), it IS a dialog the "
                f"player must dismiss -- never a silent success.")
        s3.tap("A", settle=gb_shots.BIG_SETTLE); s3.run(100)     # dismiss whichever dialog fired
        s3.shot(f"01_still_holding_{knob_name}{knob_val}",
                f"D3/R1 ({knob_name}={knob_val}): after dismissing -- footer 'A "
                f"drop  B cancel' means STILL HOLDING: the carry never completed "
                f"(lift_up() did not return true), so release_up() was never even "
                f"attempted -- the mon is SOURCE ONLY (the GB save, untouched the "
                f"whole time this display copy was held), never 'nowhere' and "
                f"never a silent duplicate.",
                claim=["A drop  B cancel"])
        changed3 = s3.vsd_report()   # report-only (no expect_changed): the exact set of
                                      # files that landed before the injected failure
                                      # fired varies per swept value BY DESIGN -- the
                                      # invariant this sweep exists to prove is the
                                      # frame's footer above, not a fixed file list
        print(f"  [VSD] D3/R1 ({knob_name}={knob_val}) on-disk diff: {sorted(changed3)} "
              f"-- whatever partial state landed here, the footer already proves the "
              f"carry itself never completed: source only, not nowhere, not a "
              f"silent duplicate")
        sessions.append(s3)

    return sessions


def run_b200_chain(core_mod, image_mod, rom: Path, out_dir: Path) -> list[gb_shots.Session]:
    """BACKLOG #200: the Gen-1/2 grid's phantom cells. `rom` MUST be the same
    flight-shaped image run_b187_chain_a() uses (Emerald.sav + a fused Yellow.gb/
    Yellow.sav, tools/fuse_sav.py then tools/fuse_gb.py, GUY'S OWN Yellow.sav
    copied to /tmp first -- the corpus at gba-toolkit/roms/gb/Yellow.sav is
    read-only) -- current box (display "12") boots 0/20 (byte 0x284C=0x8B, same
    fact run_b187_chain_a()'s own docstring records).

    Box index math (GEN1_NUM_BOXES=12, source/gen1_save.h): storage boxes are
    index 0..11 (display "1".."12"), the party pseudo-box is index 12 (13
    positions total). The boot landing is index 11 (display "12"). SWITCH_BOX's
    `(box+1) % nb` means ONE R reaches the party pseudo-box (11+1=12) and a
    SECOND R from there reaches box 1 (12+1=13 mod 13=0) -- no need to hunt for
    a full box by trial; Guy's Yellow.sav has box 1 at 20/20 (verified against
    this exact file, same as run_b187_chain_b()'s own "box1 ... genuinely 20/20
    full" claim).

    Four sub-chains, each its own Session (same posture as run_b187_chain_a/b/c):
      A: box 12 (0/20, the boot landing) -- F1's dim/X blocked cells on rows 3
         (2 blocked cells) and 4 (all 6), even though the box is EMPTY of mons
         (capacity, not occupancy, drives the paint). Also demonstrates F2's
         DOWN behaviour on column 0 (cells 0/6/12/18 real, 24 blocked): DOWN x3
         reaches the deepest real cell (18, row 3 col 0), a 4th DOWN WRAPS to
         cell 0 (row 1 col 1 in 1-based row/col terms) -- BACKLOG #198 item 8
         (the b200 review's own "wrap" one-liner): the NOT-CARRYING KEY_DOWN
         handler (source/pdna_box.c:4463-4464) is `if (src->is_bank && cur +
         COLS >= COLS*ROWS) { ...; return 5; } else cur = (cur+COLS >= cap) ?
         cur % COLS : cur + COLS;` -- at cur=18, cap=20, COLS=6, COLS*ROWS=30:
         cur+COLS=24, and 24 >= 30 is FALSE, so the off-bank-bottom `return 5`
         does NOT fire here; the else branch runs instead, and 24 >= cap(20)
         is true, so `cur = cur % COLS = 0`. The `return 5` / leave-the-grid
         reading a previous pass of this file gave belonged to a DIFFERENT
         handler entirely (:4278-4285, the CARRYING/MOVE-mode KEY_DOWN, gated
         inside `if (s_holding)`) -- box 12 chain A never picks anything up,
         so that handler is never even reached here.
      B: the GB PARTY pseudo-box (index 12, capacity 6) -- 24 of 30 cells
         blocked (every cell but row 0).
      C: box 1 (index 0, 20/20 full) -- same blocked rows 3 (partial)/4 (full)
         as box 12, but every real cell (0-19) shows an occupied Pokemon.
      D: the Emerald Gen-3 PC box (row 0 of the SAME image's boot picker,
         capacity NULL -> 30) -- unaffected: no blocked cells anywhere.
    """
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    sessions: list[gb_shots.Session] = []

    # ---- A: box 12 (0/20, boot landing) + the DOWN-off-blocked-row edge -------
    sa = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b200a_")
    print("== BACKLOG #200 Chain A: box 12 (0/20) blocked cells + DOWN edge ==")
    boot_to_gb_session(sa, rom, which="yellow")
    sa.shot("00_box12_boot", "tap0 (boot): box 12 (current box, 0/20) -- F1's dim/X "
            "blocked tiles cover row 3's last 4 cells (indices 20-23) and all of "
            "row 4 (24-29), even though the box has NO Pokemon at all -- capacity "
            "(20), not occupancy, drives the paint",
            claim=["0/20"])  # BACKLOG #184 retrofit: pdna_box.c's own banner siprintf
    sa.press_n("DOWN", 3, settle=gb_shots.SETTLE)
    sa.shot("01_col0_row3", "tap1 (DOWN x3, column 0): cursor at index 18 (row 3 "
            "col 0) -- the deepest REAL cell in this column (index 24, row 4 col "
            "0, is blocked); F2's grid_lr_step/DOWN clamp got it here one real "
            "cell at a time, same as before this lane for every cell that IS real")
    sa.tap("DOWN", settle=gb_shots.BIG_SETTLE)
    sa.shot("02_down_off_edge", "tap2 (DOWN once more, BACKLOG #198 item 8 recaption): "
            "WRAPPED to cell 0 (row 1 col 1, 1-based) -- source/pdna_box.c's own "
            "NOT-CARRYING KEY_DOWN handler (:4463-4464) is `if (src->is_bank && "
            "cur+COLS >= COLS*ROWS) return 5; else cur = (cur+COLS>=cap) ? "
            "cur%COLS : cur+COLS;`; at cur=18 the off-bank-bottom test (24>=30) "
            "is false, so the else branch fires and cur%COLS=0. VISUAL PROOF (not "
            "just the arithmetic): the L/R box-switch cursor arrow at top-left "
            "(the only visible cursor landmark -- box 12 is EMPTY, 0/20, so no "
            "mon sprite exists for a selection highlight to sit over) is back in "
            "the EXACT SAME top-left position as tap0's own boot frame (pixel-diff "
            "bbox against tap0 is a short y=22-41 strip, the top-row arrow's own "
            "bounding box -- against tap1's own cell-18 frame the diff bbox is "
            "y=22-107, spanning the full row0-to-row3 travel); confirmed by direct "
            "pixel comparison (PIL ImageChops.difference), not eyeballing alone. "
            "tap3 below moves RIGHT to make the wrap unambiguous on its own (cell "
            "0 -> cell 1), independent of the diff-bbox argument.")
    sa.tap("RIGHT", settle=gb_shots.SETTLE)
    sa.shot("03_after_reentry", "tap3 (RIGHT, BACKLOG #198 item 8 recaption): "
            "the wrapped cursor (cell 0) moves one RIGHT to cell 1 (the arrow "
            "sits under the box name's '1', one column right of tap2's own "
            "top-left landing) -- the SAME grid the whole chain has been in the "
            "entire time (this session never left pdna_box() at all: is_bank's "
            "own `return 5` branch, which WOULD have exited to the Bank hand-off, "
            "never fired -- see tap2's own caption for the exact arithmetic). "
            "Answering the brief's open question plainly: DOWN past the last "
            "real row in a capacity-bounded column WRAPS to row 0 of the SAME "
            "column (here, column 0 -> cell 0), not a leave-and-re-enter and not "
            "a stay-put.")
    sessions.append(sa)

    # ---- B: the GB PARTY pseudo-box (index 12, capacity 6) ---------------------
    sb = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b200b_")
    print("== BACKLOG #200 Chain B: the GB PARTY pseudo-box (capacity 6) ==")
    boot_to_gb_session(sb, rom, which="yellow")
    sb.tap("R", settle=gb_shots.BIG_SETTLE)
    sb.shot("00_party", "tap1 (R from box 12): the GB PARTY pseudo-box (index 12, "
            "SWITCH_BOX's (11+1)%13) -- capacity 6, so F1 blocks 24 of the 30 "
            "cells (everything past row 0)")
    sessions.append(sb)

    # ---- C: box 1 (index 0, 20/20 full) ----------------------------------------
    sc = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b200c_")
    print("== BACKLOG #200 Chain C: box 1 (20/20, full) -- same blocked rows ==")
    boot_to_gb_session(sc, rom, which="yellow")
    sc.tap("R", settle=gb_shots.BIG_SETTLE)          # box 12 -> party (index 12)
    sc.tap("R", settle=gb_shots.BIG_SETTLE)          # party -> box 1 (index 0, (12+1)%13)
    # BACKLOG #184 retrofit ATTEMPTED here (claim=["20/20"]) and REVERTED: a live
    # mGBA run against the CORPUS gba-toolkit/roms/gb/Yellow.sav (read-only, copied
    # to /tmp -- the function's own docstring warns this is NOT necessarily "GUY'S
    # OWN Yellow.sav", a different file with unverified contents) landed on a frame
    # with NO banner text at all in the top strip (a flat navy band -- the tab row
    # "<BANK>"/"SAVE" is focused/highlighted instead) -- claim_failed, but NOT
    # proven to be a caption lie: this could be the corpus save genuinely not
    # having box 1 at 20/20, or the R,R navigation landing in a tab-focused state
    # one frame earlier than this caption assumes, or a real bug. Flagged for
    # BACKLOG follow-up rather than asserted either way from this lane.
    sc.shot("00_box1_full", "tap2 (R, R from box 12): box 1, '1:GB BOX1 20/20' -- "
            "the SAME blocked rows 3 (partial)/4 (full) chain A showed on the "
            "EMPTY box 12, but every real cell (0-19) now shows an occupied "
            "Pokemon -- F1's blocked tiles are driven by capacity, not fill level")
    sessions.append(sc)

    # ---- D: the Emerald Gen-3 PC box (unaffected, capacity NULL -> 30) --------
    sd = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b200d_")
    print("== BACKLOG #200 Chain D: Emerald's own PC box -- unaffected ==")
    sd.run(700)
    sd.tap("A", settle=gb_shots.BIG_SETTLE)          # boot picker, row 0 (Emerald, default) -> PC box
    sd.shot("00_emerald_pc", "tap0 (boot, A on the default Emerald row): Emerald's "
            "own PC box screen -- BoxSource.capacity is NULL here (the Gen-3 PC/"
            "Bank source, pdna_box.h's own doc comment), so grid_capacity()/"
            "box_cap() falls back to COLS*ROWS (30) and blocked_cells() returns "
            "immediately -- no dim/X tiles anywhere, F4's 'no behaviour change "
            "for capacity == 30' claim")
    sessions.append(sd)

    return sessions
def run_s150_12_copy_edge(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #273 NOTE: the read-only START > GB import mount half described below
    (frames 02-10 as originally captured) was closed by BACKLOG #239 and is RETIRED
    from this chain -- see the comment at frame 02; only the refusal and the
    planted-cell DOWN edge (frames 11-21) still run. The prose below is kept as the
    historical record of what those retired frames showed.

    BACKLOG #150 S150-12: the read-only START > GB import mount's COPY lift, and
    a planted COPY cell's ledger-free DOWN into the Emerald PC. `rom` MUST be
    `make delta-gb`'s own combined image (Emerald.sav + Red/Gold/Crystal).

    THE WIRING PROOF (frames 05-07) -- SELECT enters MOVE on the read-only mount
    (before this lane, src_can_lift was flatly false there; decision 4's
    gb_lift_why_bs RO branch and the DRIFT-flagged gbsrc_can_enter_move_impl fix
    both had to land for this), A grabs slot 0 and the origin prompt genuinely
    draws (decision 5's g_ro_path, no NULL deref), and A picking GOLD is then
    REFUSED CLEANLY -- pdna_bank_next_serial()'s meta_save() fails (no SD card in
    mGBA, the identical wall run_s150_4_uplift() already documents for the MOVE
    lift), gb_lift_pack returns false, begin_select's refusal branch fires a beep
    only and repaints cleanly. This is the SAME "no SD card in mGBA" wall every
    write path in this tree hits -- not a delta-vehicle quirk of this lane.

    WHAT DEVIATES FROM THIS LANE'S OWN BRIEF (found live, not guessed): the brief's
    frame list expected B to first back out to the Gold info page, then a second B
    to reach Emerald's own grid with no prompt. What the vehicle actually does:
    the FIRST B only drops CM_MOVE back to CM_NORMAL (still on Gold's own grid, the
    footer changes from "MOVE A grab hold=set" back to the plain occupied-cell
    footer) -- pdna_box.c's own `if (k & KEY_B) { if (s_cur_mode != CM_NORMAL &&
    !on_title) { s_cur_mode = CM_NORMAL; ... } else { ...; return 0; } }` needs
    CM_NORMAL before B actually leaves the grid. The SECOND B then exits the whole
    GB session in one step -- pdna_box() returning 0 unwinds gb_session_core's own
    loop straight past the info page to `if (m->nblocked || m->nunreadable)
    gb_report_page(m);` (source/pdna_gen12.c), because THIS corpus's real Gold.sav
    has locked/unreadable records (the same report row-source count 5's info page
    (frame 03) already prints: "Ready to copy: 265 / Shown but locked: 15"). A
    THIRD B dismisses that report and lands on Emerald's own box grid -- with NO
    PC-offer prompt (decision 7's negative case: the lift never queued anything,
    g_pcq_count stayed 0). None of this is a defect in this lane's own code --
    gb_report_page is pre-existing, unrelated machinery this real save happens to
    trigger; captured here so the trace is honest, not force-fit to the brief's
    own guess.

    THE PLANTED-CELL DOWN EDGE (frames 11-18) proves decision 9 without needing
    the no-SD wall at all: bank_plant.c's box_load() PDNA_DELTA-only hook auto-
    plants box 0 (bank_plant_box0(), decision 17) the instant a real box0.box read
    fails -- which it always does with no SD card -- so the Bank opens showing
    "BANK 1 7/30" with the two new COPY cells already sitting at slots 5 (CHIKORITA
    L13) and 6 (L14), no live SD-writing lift required. MOVE-carrying one off the
    Bank's own bottom edge onto the Emerald PC grid and dropping it on an EMPTY
    cell (an occupied cell instead reaches drop_held's OTHER gate,
    xg_native_escape_denied, and shows "STAYS IN THE BANK" -- found live while
    probing this chain, not this lane's own gate) reaches the loss screen with the
    NOBACK rows this lane's decision 10 added, and the drop lands with no SD write
    at all (the PC placement is RAM-only until a real save, exactly like every
    other Bank->PC drop in this tree). The SECOND copy (slot 6) lands right after
    the first with NO SAVE FIRST wall between them -- decision 9's own claim that a
    copy cell has nothing to promote, so N copies may share one session.

    NOT shown here (hardware-only, not faked): a landed COPY cell surviving a real
    save, XFER-C15a's own md5-of-the-.sav proof, and the RO-mount refusal on an
    EverDrive/read-only cart (XFER-C15e) -- none of these are producible in mGBA,
    which has no SD card and no EverDrive emulation at all."""
    gb_shots.assert_vehicle(rom, "ART")  # BACKLOG #255 review: this docstring says `make delta-gb`, which is PDNA_TARGET=delta
                                    # with NO PDNA_ARTLESS=1 (Makefile:567) -- the FULL-ART delta. Asserting
                                    # ARTLESS here hard-refused the very build the chain prescribes.
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_12_")
    print("== BACKLOG #150 S150-12: the read-only mount's COPY lift + planted-cell DOWN edge ==")
    s.run(700)
    s.tap("A", settle=gb_shots.BIG_SETTLE)              # #68a boot picker, Emerald row (default) -> box
    s.shot("00_emerald_box_grid", "s150-12: Emerald's own box grid, freshly booted")

    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.shot("01_start_menu", "s150-12 (BACKLOG #198 item 9 recaption): the nav menu "
           "-- 'GB import' sits in column 2 (the RIGHT column: Blocks, Tickets, "
           "Records, Frontier, Fly, Contests, Map, GB import, Settings, Back), "
           "not column 1 -- this chain's own next tap is RIGHT before descending, "
           "which only makes sense if the target is in the second column")

    # BACKLOG #273: the nested read-only GB import mount (frames 02-10 of the old
    # chain: picker, Gold info page, RO box grid, SELECT->MOVE, origin prompt,
    # refused lift, B x3 back out) was CLOSED on purpose by BACKLOG #239 -- A on NV_GB
    # from a live Gen-3 save's nav menu now shows the "BANK ONLY" dialog and never
    # mounts anything (source/pdna_main.c app_nav_refuse()). The app is right; the
    # chain asserted a screen that no longer exists. Frames 03-10 are therefore
    # RETIRED (numbers left unused so 11+ keep their names); frame 02 now proves the
    # refusal, and the planted-cell DOWN edge below (which never needed the mount)
    # runs unchanged from frame 11.
    s.tap("RIGHT")
    s.press_n("DOWN", nav_down_from_col_top("NV_GB"))
    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("02_nv_gb_refused", "s150-12 (BACKLOG #273 re-script): A on NV_GB from the "
           "Emerald nav menu -- the 'BANK ONLY' dialog ('Open the GB save on its own,' / "
           "'send it to the Bank, come back.'), NOT the retired PICK A SAVE picker "
           "(BACKLOG #239 closed the nested mount)",
           claim=["BANK ONLY", "Open the GB save on its own,", "send it to the Bank, come back."])
    s.tap("A", settle=gb_shots.BIG_SETTLE)               # msg_wait dismisses on A
    s.shot("03_box_after_refusal", "s150-12: A dismissed the dialog -- back on Emerald's "
           "own box grid, nothing mounted, no PC-offer prompt (the retired lift never ran)")

    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.tap("DOWN", settle=gb_shots.SETTLE)                 # NV_PARTY -> NV_BANK (column 0, row 1)
    s.tap("A", settle=150)
    s.shot("11_bank_box0", "s150-12: START > Bank -- 'BANK 1  7/30', box 0's "
           "PDNA_DELTA-only auto-plant (bank_plant_box0(), decision 17) already "
           "includes the two new COPY cells at slots 5/6 -- no live SD-writing "
           "lift needed to get them here",
           claim=["7/30"])  # BACKLOG #184 retrofit: pdna_box.c's own
           # siprintf(bnocc, "%s  %d/%d", ...) -- 7 occupied of 30 capacity

    s.press_n("RIGHT", 5, settle=60)
    s.shot("12_cursor_slot5", "s150-12: cursor on slot 5 -- 'No.152 CHIKORITA "
           "Lv13 M', the first planted COPY cell (BC_FLAG_QUEUED_PC|BC_FLAG_COPY, "
           "serial 6)")

    s.tap("A", settle=150)
    s.shot("13_menu", "s150-12: the ordinary Bank-cell menu -- VIEW/EDIT, LEGALITY, MOVE, DUPLICATE, EXPORT .pk, "
           "RELEASE, CANCEL (this is a real Bank cell now, not the read-only "
           "mount's whitelist)")

    s.press_n("DOWN", 2, settle=60)                              # VIEW/EDIT -> LEGALITY -> MOVE
    s.tap("A", settle=150)                                # MOVE -> carrying
    s.shot("14_carrying", "s150-12 (BACKLOG #198 item 11 recaption): carrying "
           "the COPY cell -- the source cell (slot 5) STAYS the tinted CHIKORITA "
           "sprite with its DMG-style badge (the S150-13 lift tint -- BANK 1 "
           "still reads 7/30 here, unchanged), footer 'A drop  B cancel'; slot 5 "
           "only actually blanks ('(empty)', 6/30) after the drop lands, shown "
           "in frame 18 below",
           claim=["7/30", "A drop  B cancel"])  # BACKLOG #184 retrofit

    s.press_n("DOWN", 5, settle=150)                      # row0 -> Bank's own bottom row -> off the edge
    s.shot("15_pc_grid_carrying", "s150-12: DOWN x5 off the Bank's own bottom "
           "edge -- back on Emerald's PC grid, still carrying, box '1 5.Unp09n "
           "30/30' (full on this cartridge)")

    for _ in range(7):
        s.tap("R", settle=150)                            # R to a box with real room (found live)
    s.tap("RIGHT", settle=60)                             # slot 0 of this box may already be occupied
    s.tap("A", settle=200)                                # A on the empty cell -> the loss screen opens
    s.shot("16_loss_screen", "s150-12 decisions 9/10: A dropped on an empty PC "
           "cell reaches the honest DOWN loss screen -- 'No transfer record: "
           "this copy / cannot be sent back.' (PDNA_XFER_COPY_NOBACK_L1/_L2), "
           "alongside the ordinary 'IVs come from DVs...'/'Met: this game, "
           "traded' rows -- no ledger entry was ever written for this cell "
           "(xg_cell_is_copy() true, decision 9's own guard)",
           # BACKLOG #207: gb_down_loss_screen (source/pdna_gen12.c) had no
           # boxoam_suspend/resume bracket, so the PC box's live OBJ icons sat
           # over this dialog's text. claim= is the correct primitive here, not
           # claim_absent= -- gb_claims.find() only detects TEXT PokeDNA's own
           # font renders, it cannot "see" a sprite pixel directly, so an icon
           # drawn over this row would corrupt its EXACT glyph bitmap and make
           # this claim fail to find a match; a passing claim= is therefore
           # equally strong proof that no foreign (icon) pixels sit on this row.
           # BACKLOG #168a review (claims pass): three rows this screen always
           # draws for a copy cell -- a fixed "IVs come from DVs..." row (always
           # true), the copy-specific PDNA_XFER_COPY_NOBACK_L1 row, and the
           # bottom-row PDNA_SIDECAR_LOSS_B_CANCEL prompt -- span the icon band
           # top to bottom, same proof as the single string, three times over.
           claim=["IVs come from DVs, nature from EXP", "No transfer record: this copy", "B = cancel"])

    s.tap("A", settle=250)
    s.shot("17_landed", "s150-12: A lands the copy -- the PC box count went up "
           "by one, no SD write involved (RAM-only until a real save, same as "
           "every other Bank->PC drop)")

    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=150)
    s.press_n("RIGHT", 5, settle=60)
    s.shot("18_bank_slot5_blank", "s150-12: re-entering the Bank confirms slot "
           "5 reads blank ('(empty)') and the box count dropped from 7/30 to "
           "6/30 -- the SD-less deferred-consume path this decision reuses "
           "already ran without needing a save")

    s.tap("DOWN", settle=60)
    s.press_n("LEFT", 5, settle=60)
    s.shot("19_slot6_cursor", "s150-12: cursor on slot 6 -- 'No.152 CHIKORITA "
           "Lv14 M', the SECOND planted COPY cell (serial 7)")

    s.tap("A", settle=150)
    s.press_n("DOWN", 2, settle=60)                 # VIEW/EDIT -> LEGALITY -> MOVE (native menu, #271)
    s.tap("A", settle=150)
    s.press_n("DOWN", 4, settle=150)                      # row1 -> off the Bank's own bottom edge
    for _ in range(7):
        s.tap("R", settle=150)
    # NOTE (found live): unlike the first drop, the box's own remembered cursor
    # already rests on an empty cell here (the first copy's landing slot shifted
    # it) -- no extra RIGHT needed; an extra RIGHT here would land back on an
    # OCCUPIED cell and hit xg_native_escape_denied's "STAYS IN THE BANK" refusal
    # instead (found live while building this chain, not this lane's own gate).
    s.tap("A", settle=200)
    s.shot("20_second_loss_screen", "s150-12 decision 9: the SECOND copy's own "
           "DOWN reaches the SAME honest loss screen -- critically, NO 'SAVE "
           "FIRST' wall between the two drops (a copy has nothing to promote, "
           "so N copies may land in one session, unlike an ordinary S150-8d "
           "ledger-pending cell)",
           # BACKLOG #207 (same proof as frame 16 above, re-run on the SECOND
           # copy's own loss screen).
           claim=["IVs come from DVs, nature from EXP", "No transfer record: this copy", "B = cancel"])

    s.tap("A", settle=250)
    s.shot("21_second_landed", "s150-12: the second copy lands too -- both "
           "planted cells now sit in the Emerald PC, proving decision 9's DOWN-"
           "skips-the-ledger claim end to end on the emulator")

    return s


def run_s150_11_reconcile(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-11: the TRANSFERS screen (decision 13/14) -- the 21-row START
    menu, the empty-state screen, a Game Boy session's dimmed row + honest refusal, and
    a silent Bank open. `rom` MUST be `make delta-gb`'s own combined image (Emerald.sav
    + Red/Gold/Crystal, same vehicle as --s150-12) -- the chain needs BOTH the Gen-3
    side (frames 00-03, 06) and a Game Boy session (frames 04-05), which S150-14's own
    single-Emerald fusion (no --gb) cannot provide (no boot picker, no GB row at all).

    WHAT DEVIATES FROM THIS LANE'S CONTINUATION BRIEF (found live, not guessed): the
    brief's step 7 said to use S150-9's bank_plant_xfer_seed_all() delta slots (box-0
    slots 26-29) "so the list has real rows". Checked before this ladder ran a single
    tap: source/xfer_io.c's own PDNA_DELTA comment says plainly "the delta vehicle has
    no readable FAT" -- the seam bank_plant_xfer_seed_all()/xfer_io.c's PDNA_DELTA
    fallback provide is a single BY-KEY lookup (consumed by source/xfer_view.c's GB
    ORIGINAL row through xr_open()), not a directory listing. source/pdna_main.c's
    xfer_reconcile_walk() -- this screen's own walk -- calls f_opendir(&dir,
    PDNA_XFER_DIR) directly (never xr_open()), which fails on this vehicle regardless
    of what is planted in the Bank: rb->nxrc stays 0 no matter how many Bank cells
    bank_plant_xfer_seed_all() plants. Decision 16 ("delta vehicle = the empty state
    only... do NOT add a RAM-backed ledger or #ifdef PDNA_DELTA plants for records --
    S150-13 owns #179") is therefore still exactly right for this screen's LIST view,
    confirmed against the shipped source rather than assumed; a populated-list shot
    needs either S150-13's real RAM ledger or real hardware (BACKLOG #150 S150-11's
    HW-QUEUE rows XFER-C13/C14/C16b/C20/C27/C28/C29 cover the populated cases)."""
    gb_shots.assert_vehicle(rom, "ART")  # BACKLOG #255 review: this docstring says `make delta-gb`, which is PDNA_TARGET=delta
                                    # with NO PDNA_ARTLESS=1 (Makefile:567) -- the FULL-ART delta. Asserting
                                    # ARTLESS here hard-refused the very build the chain prescribes.
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_11_")
    print("== BACKLOG #150 S150-11: the TRANSFERS screen (START row, empty state, GB "
          "session refusal, silent Bank open) ==")

    # BACKLOG #277: the Gold session used to be reached through the nested NV_GB import
    # (closed by BACKLOG #239). It is now reached the way the boot picker reaches it --
    # so this half runs FIRST, from the boot picker's Gold row, then backs out through the
    # picker to Emerald's box grid for the Gen-3 frames (01-04, 08). Frame NAMES keep
    # their old numbers; capture order is 05, 06, then 01..04, 08 (old frame 07, the
    # no-residue check, is folded into 01's caption).
    s.run(700)
    boot_to_gb_session(s, rom, which="gold")          # picker -> Gold row -> box grid (#279: no info page)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.tap("RIGHT")
    s.press_n("DOWN", nav_down_from_col_top("NV_XFER"))
    s.shot("05_gb_session_start", "s150-11 decision 4/G-F2: the SAME 21-row menu "
           "inside a Gold session, cursor already on 'Transfers' -- the CURSOR "
           "highlight paints selected text the same bright ink whether the row is "
           "enabled or not (the same convention 02's own frame shows for the Gen-3 "
           "menu -- caption honest: the screenshot alone does not visually "
           "distinguish dimmed-but-selected from enabled-and-selected; the actual "
           "NAV_COMING_SOON classification is nav_avail.c's GB_TABLE[NV_XFER] row, "
           "pinned by tests/host_nav_avail_test.c, and frame 06 is the real proof "
           "-- A here refuses instead of opening the screen)")

    s.tap("A", settle=200)
    s.shot("06_gb_session_refused", "s150-11: A on the dimmed row -- the honest "
           "'Open it from a Gen-3 save.' refusal (app_nav_refuse(), the SAME "
           "message nav_avail.c's [NV_XFER] table entry carries); nothing was "
           "read from the ledger and nothing was logged (G-F2 -- verified "
           "structurally by tests/host_xfer_reconcile_sites_test.py's check (i), "
           "not re-provable from a screenshot alone). Dialog title is 'COMING "
           "SOON' (app_nav_refuse()'s NAV_COMING_SOON branch, source/pdna_main.c "
           "~9528), NOT the body line quoted above.", claim="COMING SOON")

    # A dismisses msg_wait (KEY_A only). Leaving the Gold box grid (B) surfaces this
    # corpus's held-item "NOT TRANSFERABLE" report, a second B closes the session and
    # returns to the boot picker (asserted, not assumed), and B there resolves to the
    # Emerald row.
    s.tap("A", settle=200)
    s.tap("B", settle=200)
    s.tap("B", settle=200)
    assert_screen(s, "pick_a_save")
    s.tap("B", settle=gb_shots.BIG_SETTLE)
    s.shot("01_gen3_box", "s150-11: the Emerald box screen -- reached by backing all the "
           "way out of the Gold session (A dismissed the refusal, B and B: one dismisses "
           "this corpus's unrelated 'NOT TRANSFERABLE' held-item report, the second exits "
           "the session to the boot picker, asserted) and B on the picker = Emerald row. "
           "Doubles as the no-residue check for the GB-session detour (old frame 07)")

    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.tap("RIGHT")                                       # column 0 -> column 1
    s.press_n("DOWN", nav_down_from_col_top("NV_XFER"))  # column 1 top -> Transfers' own row
    s.shot("02_start_menu_21_rows", "s150-11 decision 13: the 21-row START menu -- "
           "'Transfers' now sits between 'GB import' and 'Settings' in column 2, "
           "cursor already on it (PDNA_NAV_ROW_H 11 -> 10 fits the 21st row -- the "
           "hint line at the bottom of the panel sits clear of the last row, not "
           "overlapping it)", claim="Transfers")

    s.tap("A", settle=gb_shots.BIG_SETTLE)
    s.shot("03_transfers_empty", "s150-11 decision 16: A on Transfers -- the "
           "empty-state screen ('No transfer records. / Records appear after a / "
           "Bank transfer.') -- this delta vehicle has no readable FAT "
           "(source/xfer_io.c), so xfer_reconcile_walk()'s f_opendir() always "
           "fails here regardless of what is planted in the Bank; this is the "
           "ONLY reachable frame for this screen on the emulator (decision 16)",
           claim="TRANSFER RECORDS")

    s.tap("B", settle=gb_shots.BIG_SETTLE)
    s.shot("04_transfers_back", "s150-11: B backs out of the empty-state screen "
           "-- Emerald's own box screen again, no residue")

    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.tap("DOWN", settle=gb_shots.SETTLE)                 # NV_PARTY -> NV_BANK (column 0, row 1)
    s.tap("A", settle=200)
    s.shot("08_bank_open_silent", "s150-11 decision 4/§11.8: START > Bank opens "
           "with NO prompt -- xfer_reconcile_walk()'s own f_opendir() fails "
           "silently on this vehicle's unreadable FAT (same finding as frame 03), "
           "so app_xfer_reconcile_bank_open() always sees zero candidates here; "
           "the log-line assertion (XFER-C13/C14) is hardware-only, not "
           "producible on the emulator")
    return s


def run_b222_summary_nick(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #222 review R1/R3(a): the Gen-1/2 summary's own Nickname row through the
    NEW fixed-font e-acute/gender-sign collapse (ui_ascii_next_fixed(), source/
    ui_ascii.c). `rom` must be a single-ROM fused image (tools/fuse_gb.py, one Red.gb +
    one edited Red.sav, on a `make delta-artless` base -- no Emerald.sav, so the boot
    picker is skipped) whose box slot 0 was renamed PIKA♂ via tests/
    host_gbsurgery_tool.c's `--op nick 0 0 "PIKA♂"` (gb_edit.c's enc_one() maps the
    UTF-8 gender-sign pair E2 99 82 to the Game Boy raw nickname byte 0xEF -- a save-
    format encoding, unrelated to ui_ascii's own collapse, which only runs on the
    DECODED UTF-8 string source/pdna_gbsummary.c hands to ui_text()).

    Nav: boot_to_gb_session() lands on the box grid (single-ROM image, no picker, the
    rom_gbsprite cold scan already ridden out). Slot 0 is occupied (this corpus's every
    box is full) -- A opens its own cell menu (VIEW/EDIT already selected, row 0), A
    again enters the summary in VIEW mode, Card 0 INFO (gb_shots.py's run_red/run_gold
    calibrate the identical two-A nav). card_info() (source/pdna_gbsummary.c) draws
    Nickname first via field_row() -> ui_text(), which since review R1 bounds through
    ui_ascii_next_fixed() -- sys8's own cell 127 is a blank 8x8 tile (BACKLOG #222 R1),
    so PIKA♂'s decoded gender sign collapses to '?': "PIKA?"."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b222_nick_")
    print("== BACKLOG #222 review R3(a): the Gen-1/2 summary's Nickname row, PIKA? ==")
    boot_to_gb_session(s, rom)
    s.tap("A", settle=gb_shots.BIG_SETTLE)   # box grid, slot 0 occupied -> its own cell menu
    s.tap("A", settle=gb_shots.BIG_SETTLE)   # VIEW/EDIT (row 0, already selected) -> the summary
    s.shot("01_nick_row", "BACKLOG #222 R1/R3(a): Card 0 INFO, Nickname row -- "
           "PIKA♂ (stored as Game Boy raw byte 0xEF) decodes to UTF-8 and "
           "collapses through ui_ascii_next_fixed() to 'PIKA?' (sys8's cell 127 is "
           "blank, so the FIXED-font path can show neither e-acute nor the gender "
           "signs -- only '?', unlike the proportional font's own real 127 glyph)",
           claim=["PIKA?"])
    return s


def run_b216b_summary_cafe(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #216b: the same site run_b222_summary_nick() shoots, now on a Gold box
    mon renamed "CAFé" via tests/host_gbsurgery_tool.c's `--op nick 0 0` (gb_edit.c's
    enc_one encodes the UTF-8 "\xC3\xA9" to the real Gen-2 byte 0xEA, exactly the byte
    this lane's gen2_save.c fix now decodes back to "CAFé" instead of folding it to a
    plain 'e'). `rom` must be a single-ROM fused image (tools/fuse_gb.py, one Gold.gbc
    + that edited Gold.sav, on a `make delta-artless` base -- no Emerald.sav, so the
    boot picker is skipped).

    CORRECTED FROM THE FIRST DRAFT (this docstring claimed no proportional row exists
    here; the captured frame proved otherwise -- read it, don't guess): this SCREEN
    draws the nickname TWICE, through two independently-fed paths. (1) The RIGHT panel
    Card 0 INFO's own Nickname row -- card_info()'s field_row() -> ui_text() (the fixed
    font) -- collapses to "CAF?" (BACKLOG #222's ui_ascii_next_fixed(), sys8 cell 127
    is blank). (2) The LEFT panel is a THROWAWAY Gen-3-style conversion shared with
    every Gen-3 summary (pdna_gbsummary.c's gbsum_convert_left() -> pdna_summary.c's
    draw_left_ex(), source/pdna_summary.c:213 `ui_ptext_fit(..., p->nickname, ...)`,
    the PROPORTIONAL font) and shows the real "CAFé" -- font code 127 is a real glyph
    on that path (BACKLOG #216's own pnext() special case), not blank.

    NEITHER draw site reads through gen1_save.c/gen2_save.c (this lane's changed
    files): both panels feed off gbsum_convert_left()'s own `gb_get_nickname(e, ...)`
    (source/pdna_gbsummary.c:172), which is gb_edit.c's gb_name_decode -- already
    UTF-8-correct before this lane touched anything. So this frame demonstrates the
    PREDICTED fixed-vs-proportional split (the brief's own question, "what does ui_ptext
    draw for the umlauts/é" -- answered: é, yes; the umlauts still render '?', no font
    glyph for them at all, display-only, unrelated to this lane's decoder fix) but is
    NOT itself evidence for the gen1_save.c/gen2_save.c change -- that is what tests/
    host_xferdown_test.c's test_cafe_umlaut_bridge_2_3_2 and tools/gb_retail_gate.py's
    run_cafe_case (the real boot readback via --list, gb_edit.c's own decoder) prove.

    Nav: identical to run_b222_summary_nick() -- boot_to_gb_session() lands on the box
    grid (single-ROM image, no picker), A opens slot 0's own cell menu (VIEW/EDIT
    already selected, row 0), A again enters the summary in VIEW mode, Card 0 INFO."""
    gb_shots.assert_vehicle(rom, "ARTLESS")  # BACKLOG #255: this chain's own docstring names a required vehicle
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b216b_cafe_")
    print("== BACKLOG #216b: the Gen-1/2 summary's Nickname row, a Gold CAFé mon ==")
    boot_to_gb_session(s, rom)
    s.tap("A", settle=gb_shots.BIG_SETTLE)   # box grid, slot 0 occupied -> its own cell menu
    s.tap("A", settle=gb_shots.BIG_SETTLE)   # VIEW/EDIT (row 0, already selected) -> the summary
    s.shot("01_nick_row", "BACKLOG #216b: Card 0 INFO -- a Gold box mon renamed CAFé "
           "(Gen-2 raw byte 0xEA). The RIGHT panel's own Nickname row (fixed font, "
           "ui_text) shows 'CAF?' (BACKLOG #222's sys8-cell-127-is-blank collapse). "
           "The LEFT panel (a throwaway Gen-3-style preview, proportional font, "
           "ui_ptext_fit) shows the real 'CAFé' -- font code 127 IS a real glyph on "
           "that path. Both panels read the SAME already-correct gb_edit.c decoder "
           "(unrelated to this lane's gen1_save.c/gen2_save.c fix); the fix itself is "
           "proven by tests/host_xferdown_test.c + tools/gb_retail_gate.py's readback, "
           "not by this screen.",
           # NOTE: gb_claims.py's render() only covers ASCII 0x20-0x7f (its own
           # ValueError, hit while drafting this case) -- "CAFé" cannot be an
           # automated claim= target at all; the left panel's real é is verified BY
           # EYE (docs/briefs/b216b.md's own executor report), not mechanically here.
           claim=["CAF?"])
    return s


def run_b222_hof_ot(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #222 review R1/R3(b): the Hall of Fame team detail's own nickname line,
    through the SAME fixed-font collapse as R3(a) above, but reached via the NO-ROM
    plain fallback this time (source/pdna_gbhof.c's hof_plain_screen() ->
    hof_detail_visit() -> hof_detail_render(), plain ui_text(), NOT the GB-shell's own
    composited gbscr_text() font). `rom` must be fused with NO Gen ROM at all
    (tools/fuse_gb.py fed only the edited Red.sav -- the same 'no ROM' construction
    run_b194_hof_no_rom() above uses) so gbscr_open() refuses (kReasonNoRom) and
    pdna_gbhof() falls straight through to the plain page. The embedded save's Hall of
    Fame team 0 mon 0 (MEW, verified against this exact corpus by run_b89_hof_detail_
    only()'s own docstring) was renamed PIKA♂ via tests/host_gbsurgery_tool.c's
    `--op hofnick 0 0 "PIKA♂"`.

    Nav: boot_to_gb_session() lands on the box grid (single-save image, no picker) ->
    START -> nav menu -> DOWN x12 (Records, PDNA_NAV_ITEMS index 12 -- the same count
    run_b194_hof_no_rom() uses) -> A (Records -> pdna_gbhof() -> gbscr_open() refuses
    immediately, no ROM to scan, straight to hof_plain_screen()'s own row list) -> A
    (team 1's row, sel=0 by default -> hof_detail_visit() -> hof_detail_render())."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b222_hof_")
    print("== BACKLOG #222 review R3(b): the HoF detail's nickname line, no ROM, PIKA? ==")
    boot_to_gb_session(s, rom)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 12)
    s.tap("A", settle=gb_shots.BIG_SETTLE)   # Records -> pdna_gbhof() -> refuses (no ROM) -> plain list
    s.tap("A", settle=gb_shots.BIG_SETTLE)   # team 1's row -> hof_detail_visit() -> hof_detail_render()
    s.shot("01_hof_nick", "BACKLOG #222 R1/R3(b): hof_detail_render's nickname line "
           "for team 1 mon 0 (MEW, renamed PIKA♂) -- the SAME "
           "ui_ascii_next_fixed() collapse as the summary screen, drawn through "
           "plain ui_text() (no GB-shell composited font here at all, since no ROM "
           "is fused) -- 'PIKA?'",
           claim=["PIKA?"])
    return s


def run_b222_bag_item(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #222 review R1/R3(c): the Gen-1/2 Item bag's own no-ROM plain fallback
    (pdna_gbbag_plain() -> gbbag_row_paint(), source/pdna_gbbag.c) drawing a REAL item
    name through ui_text() -- R1's fixed-font collapse applies here too, on a name this
    tree ships itself (gb_item_names.c, GREEN per licensing), not a planted string.
    `rom` must be fused with NO Gen ROM at all (the same 'no ROM' construction R3(b)
    above uses) so gbscr_open() refuses and pdna_gbbag() falls back to pdna_gbbag_
    plain(). The embedded save's ITEMS pocket had id 4 ("POKé BALL",
    gb_item_names.c index 4) inserted at qty 10 via tests/host_gbsurgery_tool.c's
    `--op item items 4 10`: gbb_insert() (source/gb_bag.c) always APPENDS a new id at
    list->count, never sorts -- verified against this exact fixture with a throwaway
    gbb_read()/gb_item_label() dump (not shipped): the corpus's Items pocket already
    held 19 entries, so the new POKé BALL landed at row 19 of 20 (0-based), the
    LAST row, not anywhere earlier.

    Nav: boot_to_gb_session() lands on the box grid (single-save image, no picker) ->
    START -> nav menu -> DOWN x7 (Bag, PDNA_NAV_ITEMS index 7 -- the same count run_
    u4_bag() uses) -> A (Bag -> pdna_gbbag() -> gbscr_open() refuses (no ROM),
    straight to pdna_gbbag_plain(), no cold-scan settle needed since there is no ROM
    to scan -- BIG_SETTLE, not GB_ART_COLD_SETTLE, matching run_b194_hof_no_rom()'s
    own no-ROM reasoning) -> DOWN x19 (row 0 -> row 19; gbbag_row_paint()'s own top-
    follows-sel scroll keeps the selected row on screen throughout, landing on the
    inserted entry as the LAST visible row once top settles at 8)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "b222_bag_")
    print("== BACKLOG #222 review R3(c): the bag's plain fallback, POKé BALL -> POK? BALL ==")
    boot_to_gb_session(s, rom)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 7)
    s.tap("A", settle=gb_shots.BIG_SETTLE)   # Bag -> pdna_gbbag() -> refuses (no ROM) -> plain list
    s.press_n("DOWN", 19)                    # row 0 -> row 19 (the newly-inserted POKe BALL)
    s.shot("01_bag_item", "BACKLOG #222 R1/R3(c): the plain fallback's row 19, id 4 "
           "(\"POKé BALL\", gb_item_label()) drawn through gbbag_row_paint()'s "
           "ui_text() -- collapses to 'POK? BALL' (the SAME sys8-blank-127 fact R1 "
           "documents, so the fixed font cannot show e-acute here either)",
           claim=["POK? BALL"])
    return s


if __name__ == "__main__":
    sys.exit(main())
