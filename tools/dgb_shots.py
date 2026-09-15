#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""dgb_shots.py — re-run the GB shot flows on pokedna-delta-gb.gba (BACKLOG #62, #68a).

WHY A SEPARATE SCRIPT (does NOT edit tools/gb_shots.py)
---------------------------------------------------------
tools/gb_shots.py drives PokeDNA's Game Boy fork against a build fused with exactly
ONE GB save (tools/fuse_sav.py --gb) — boot lands straight on S1's info page via
view_save()'s own standalone GB-size fork (memset g_vinfo, pdna_gen12_show_image over
a resident buffer): a full read/write-capable mount (VIEW/EDIT combined, MOVE TO BOX,
RELEASE, CREATE on an empty cell). pokedna-delta-gb.gba (tools/fuse_gb.py, BACKLOG #62)
fuses Emerald.sav (the Gen-3 flash seed) TOGETHER WITH the Red/Gold/Crystal directory.

#68a: view_save() now offers a BOOT PICKER whenever a Gen-3 save is READY -- the flash
chip parses, or (the normal case on a fresh emulator boot, where the flash is blank) the
single-slot fused .sav fallback supplies it -- AND a GB corpus is fused in (gb_delta_boot_pick(), source/pdna_main.c) — row 0 is
the loaded Gen-3 save, rows 1..n mirror the fused GB saves one for one. Picking the
Gen-3 row continues into the normal Emerald box screen, where the nav menu's "GB
import" row (NV_GB) still offers the READ-ONLY nested mount via
pdna_gen12_show_fused() — VIEW / LEGALITY / COPY / CANCEL, no EDIT, no MOVE TO BOX, no
RELEASE, no CREATE. Picking a GB row instead reuses the SAME full read/write STANDALONE
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
"""
from __future__ import annotations

import argparse
import functools
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots  # noqa: E402 -- Session, load_mgba, KEY, HOLD/SETTLE/BIG_SETTLE
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
# follows for the GB field tables; (2) nav_to_gb_import() below asserts the landed
# screen against the SAME "PICK A SAVE" crop-signature boot_to_gb_session() uses --
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


def nav_to_gb_import(s: gb_shots.Session) -> None:
    """START (box grid -> nav menu) -> RIGHT (column 0 -> column 1) -> DOWN x(NV_GB's
    own row, derived from source every run) -> A -> asserts the landing is really
    the "PICK A SAVE" picker (gb_delta_pick_save(), the SAME screen boot_to_gb_
    session() lands on at the top-level boot fork) before returning -- a stale/wrong
    DOWN count that lands on a DIFFERENT SCREEN now raises here, with a screenshot, instead of silently shooting
    whatever screen it actually lands on (BACKLOG #118 coordinator addition)."""
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box screen -> nav menu
    s.tap("RIGHT")                                            # column 0 (Party) -> column 1 (Blocks)
    s.press_n("DOWN", nav_down_from_col_top("NV_GB"))          # Blocks -> ... -> GB import
    s.tap("A", settle=gb_shots.BIG_SETTLE)                    # NV_GB -> the (separate) nested-import picker
    assert_screen(s, "pick_a_save")

# #62 review D9: rides out the one-time, per-generation cold ROM scan (see module
# docstring). Gen 1 (a single sprite-portrait scan) measured ~258 s; Gen 2 pays TWO
# independent cold scans the first time a box grid needs BOTH the left-panel portrait
# (gb_art_fetch, its own s_dsprite_loc cache) and the grid's own 16x16 menu icons
# (gb_art_fetch_icon, its own separate s_dicon_loc cache) -- measured ~500 s. 32,000
# frames = ~533 s of emulated GBA time, comfortably over the Gen-2 worst case.
GB_ART_COLD_SETTLE = 32000

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
        landing straight on that save's own "GAME BOY SAVE" info page the
        instant `s.run(700)` finishes (verified: Red-only and Gold-only images
        both show "GAME BOY SAVE" with zero picker frames).
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
        s.tap("A", settle=60)                               # pick row -> S1 info
    assert_screen(s, "gb_info_page")
    s.tap("A", settle=60)                    # S1 info -> box grid (rom_gbsprite cold fetch)
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


def run_nav_gb(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """pokedna-delta-gb.gba: boots into #68a's boot picker (row 0 = Emerald.sav, the
    valid flash save; rows 1..3 = the fused GB corpus) -- A on the default Emerald
    selection continues into the ordinary Gen-3 box screen, from which this drives the
    nav menu to NV_GB, opens the (separate) nested-import picker, picks `which`, and
    captures what the nested-import mount actually shows: the info page, the box grid
    (real Game Boy art -- BACKLOG #62's whole point, cold-fetched the first time), the
    occupied-cell menu, and VIEW (a real portrait via the shared origin-art router)."""
    idx = gb_save_pick_index(rom)[which]
    # "dgb_" prefix: tools/gb_shots.py's own run_gold()/run_red() already claim
    # gold_01_info.png etc. for the single-fused-save build's standalone S1..S5 flow --
    # this build's shots are a DIFFERENT screen (the nested NV_GB import mount, see
    # module docstring) and must not collide with or overwrite that existing evidence.
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"dgb_{which}_")
    print(f"== delta-gb / {which}: NV_GB import (real art) ==")

    # Session.__init__'s own 180-frame settle is calibrated against a plain single-
    # fused-save image (gb_shots.py's own targets); this recipe's Emerald.sav + the
    # whole GB directory boots slower (calibrated by hand: still on "Opening save...
    # N/13 first paint: box" at 180 frames) -- extra settle before the first tap so
    # A actually lands past the boot picker, not mid-load.
    s.run(700)
    s.tap("A", settle=gb_shots.BIG_SETTLE)              # #68a boot picker, Emerald row (default) -> box
    nav_to_gb_import(s)                                   # box screen -> nav menu -> NV_GB -> the save picker
    for _ in range(idx):
        s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # picked -> this save's own S1 info page
    s.shot("01_info", f"#62: {which}.sav — S1 info page, opened via NV_GB "
                       "(pdna_gen12_show_fused, read straight out of cartridge space)")

    s.tap("A", settle=60)                                 # info -> box grid (COLD fetch starts)
    s.run(GB_ART_COLD_SETTLE)                             # ride out the first-ever real scan
    s.shot("02_box_grid", f"#62: {which}.sav — box grid with REAL {which.upper()} art: "
                           "sprites decoded straight out of the fused ROM (D1/E3), the same "
                           "16x16 menu icons Gen-2's own PC uses where this save is Gen 2 (E5). "
                           "The portrait is a 4-shade Game Boy sprite, not the full-colour "
                           "Gen-3 stand-in a build without D1's fix would show here.")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                # A on the box's own first mon -> its menu
    s.shot("03_mon_menu", f"#62: {which}.sav — the occupied-cell menu this NESTED IMPORT mount "
                           "offers: VIEW / LEGALITY / COPY / CANCEL only -- no EDIT, MOVE TO BOX, "
                           "or RELEASE (those exist on the STANDALONE mount view_save()'s own GB-"
                           "size fork uses, which this recipe's fused Emerald.sav makes "
                           "unreachable at boot -- see this file's own module docstring)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                # VIEW -> the native summary (read-only)
    s.shot("04_view_summary", f"#62: {which}.sav — VIEW: the native read-only summary with a "
                               "REAL portrait (pdna_origin_art's GB router now reads the fused "
                               "ROM instead of falling back to Gen-3 stand-in art)")
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

    Captures Trainer / Bag-or-Pack / Flags / Dex / Map from BOTH of THIS image's
    real mounts over the SAME `which` save, named to `cmp` pairwise:
      - `import_*`  the nested NV_GB import (nav_to_gb_import(), THIS lane's own
                     streamed read-only session, ed == false, gs == &vw.s)
      - `direct_*`  the boot picker's own DIRECT GB row (boot_to_gb_session(), the
                     pre-existing resident-image mount, ed == true, gs == &g_ed->s)
    Same underlying save bytes either way -- a real pixel difference here would
    mean either the streamed session's read parity is wrong, or the read-only
    render draws different chrome than the editable one (worth a caption either
    way, not necessarily a bug: e.g. an edit cursor only the editable mount shows)."""
    idx = gb_save_pick_index(rom)[which]
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

    # ---- mount 1: the nested NV_GB import (streamed, ed == false) --------------
    s1 = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b64_{which}_import_")
    print(f"== BACKLOG #64 Fix 3: {which}.sav — NV_GB import mount (streamed, ed==false) ==")
    s1.run(700)
    s1.tap("A", settle=gb_shots.BIG_SETTLE)                # #68a boot picker, Emerald row (default) -> box
    nav_to_gb_import(s1)                                     # box screen -> nav menu -> NV_GB -> the save picker
    for _ in range(idx):
        s1.tap("DOWN", settle=gb_shots.SETTLE)
    s1.tap("A", settle=gb_shots.BIG_SETTLE)                  # picked -> this save's own S1 info page
    s1.tap("A", settle=60)                                   # info -> box grid (COLD fetch starts)
    s1.run(GB_ART_COLD_SETTLE)                               # ride out the first-ever real scan
    for i, (tag, n, a_settle) in enumerate(rows):
        s1.tap("START", settle=gb_shots.BIG_SETTLE)
        s1.press_n("DOWN", n)
        s1.tap("A", settle=a_settle)
        if tag == "dex" and not kind_g1:
            # BACKLOG #64 review R2: Gen 2 lands on gbdex_chooser() first (Pokedex /
            # Unown forms, row 0 default-selected) -- one more A confirms row 0 and
            # opens the actual dex grid, so this shot matches red's (Gen 1's own,
            # chooser-free) landing screen shape. The grid's OWN header row ("No.1
            # BULBASAUR ...") draws one repaint pass behind BIG_SETTLE (hand-
            # calibrated, same class of delayed chrome as the box grid's own) --
            # without this extra run(), the shot (and the B-count below) land on a
            # stale partial frame that looks like an extra chooser round trip.
            s1.tap("A", settle=gb_shots.BIG_SETTLE)
            s1.run(2000)
        s1.shot(f"{i + 1:02d}_{tag}",
                f"#64 Fix 3: {which}.sav via NV_GB's fused mount -- {tag} row -- NOW A REAL "
                "streamed (read-only) session (gbs_open_streamed over the same "
                "fused_gb_slice_read pair the mount used), not the gb_info_page fallback "
                "this lane used to leave here before Fix 3")
        s1.tap("B", settle=gb_shots.BIG_SETTLE)
        if tag == "dex" and not kind_g1:
            s1.tap("B", settle=gb_shots.BIG_SETTLE)   # grid -> chooser -> box grid (2 levels)
        s1.run(2000)   # box grid header/footer repaint settle (hand-calibrated, see git log)
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

    print(f"\n== BACKLOG #64: {which}.sav -- cmp import_* vs direct_* ==")
    for i, (tag, _n, _a) in enumerate(rows):
        a = out_dir / f"b64_{which}_import_{i + 1:02d}_{tag}.png"
        b = out_dir / f"b64_{which}_direct_{i + 1:02d}_{tag}.png"
        if not (a.exists() and b.exists()):
            print(f"  [skip] {tag}: missing {a if not a.exists() else b}")
            continue
        same = a.read_bytes() == b.read_bytes()
        print(f"  {'MATCH ' if same else 'DIFFER'} {tag}: {a.name} vs {b.name}")

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
    s.tap("A", settle=60)                                 # pick Red -> S1
    s.shot("02_s1_info", "#68a: Red.sav's own S1 info page -- the standalone "
                          "mount, reached via the boot picker's Red row")

    s.tap("A", settle=60)                                 # -> box grid (COLD fetch, gen1 loc empty)
    s.run(GB_ART_COLD_SETTLE)
    s.shot("03_box_grid", "#62 D1: box grid with REAL Red/Blue/Yellow art -- 4-shade Game Boy "
                           "sprites decoded from the fused Red.gb ROM, the first-ever cold scan "
                           "(D9: ~258 s of emulated GBA time) already ridden out")

    # This corpus's every box is full -- RELEASE the box's own first mon (top-left) to
    # open a slot CREATE can use. Menu order: VIEW/EDIT, LEGALITY, MOVE TO BOX, COPY,
    # RELEASE, CANCEL (4 DOWNs from the top to RELEASE).
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # cell menu
    s.press_n("DOWN", 4, settle=gb_shots.SETTLE)          # -> RELEASE
    s.shot("04_release_menu", "#62 A3: the occupied-cell menu the STANDALONE mount offers -- "
                               "VIEW/EDIT, LEGALITY, MOVE TO BOX, COPY, RELEASE, CANCEL (the "
                               "nested-import mount's VIEW/LEGALITY/COPY/CANCEL, plus every "
                               "write action, since this session can actually edit)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # "Release this Pokemon?"
    s.shot("05_release_confirm", "#62: the release confirm dialog")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # A = yes -> gb_persist() -> PDNA_DELTA refusal
    s.shot("06_refusal", "#62 D2/D5: gb_persist()'s PDNA_DELTA branch -- 'Edits are in-session "
                          "only in the emulator build.' The release is NOT written anywhere "
                          "(there is no SD card, and this build's flash chip stays blank "
                          "either way), but D2's fix means it also is NOT lost: pristine is "
                          "re-baselined to the post-release image right here.")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # dismiss -> back at the box, now 19/20

    # Navigate to the freed cell: this cursor position is this specific corpus's own
    # empty slot after ONE release from the box's top-left mon -- calibrated by hand
    # against Guy's real Red.gb/Red.sav dump, not a general rule about box layout.
    s.tap("UP", settle=gb_shots.SETTLE)
    s.tap("UP", settle=gb_shots.SETTLE)
    s.press_n("LEFT", 6, settle=gb_shots.SETTLE)
    s.press_n("DOWN", 3, settle=gb_shots.SETTLE)
    s.shot("07_empty_cell", "#62: cursor on the freshly-released, now-empty cell (19/20)")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                # empty-cell menu: EMPTY / CREATE / CANCEL
    s.shot("08_create_menu", "#62 A3: the empty-cell menu -- EMPTY (header) / CREATE / CANCEL")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # select CREATE -> pick_species(1)
    s.shot("09_species_picker", "#62: CREATE opens the same pdna_pick.c pick_species() screen "
                                 "Gen-3's own app_create_mon uses -- Bulbasaur (dex 1) "
                                 "pre-selected")

    s.tap("A", settle=60)                                 # pick Bulbasaur -> gb_create_learn (ROM read)
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
    s.tap("A", settle=gb_shots.BIG_SETTLE)                # dismiss -> box grid, new mon showing
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

    # NV_GB nested import, exercised from INSIDE this same Emerald session, to prove it
    # is unaffected by #68a's boot picker: same navigation run_nav_gb() below uses, but
    # inline here so one continuous session covers both the new picker AND the existing
    # nested-import path in one screenshot run.
    nav_to_gb_import(s)                                     # box screen -> nav menu -> NV_GB -> the picker
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # Red (index 0) -> Gold (index 1)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # picked -> Gold's own S1 info page
    s.shot("17_nv_gb_info", "#68a: NV_GB's nested import still works from inside the "
                             "Emerald session after #68a -- Gold.sav's S1 info page via "
                             "pdna_gen12_show_fused(), untouched by the boot-picker change")
    s.tap("A", settle=60)                                   # info -> box grid (COLD fetch, gen2)
    s.run(GB_ART_COLD_SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the box's own first mon -> its menu
    s.shot("18_nv_gb_mon_menu", "#68a: the nested import's occupied-cell menu is still "
                                 "VIEW / LEGALITY / COPY / CANCEL only -- no EDIT, MOVE TO BOX, "
                                 "or RELEASE (those exist only on the STANDALONE mount reached "
                                 "through the boot picker's GB rows, shots 02-14 above)")
    return s


def run_u2c_trainer(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """U2c (docs/GB-GAME-SCREENS-DESIGN.md sec 1.1): Red's OWN trainer card on the
    shared GB-screen shell, over the STANDALONE mount (boot picker DOWN -> Red row ->
    A -> S1 info -- same path run_standalone()'s own shots 01-02 use), then A -> box
    grid (rides out rom_gbsprite's cold scan the same way run_standalone()'s own
    03_box_grid does), START -> nav menu, DOWN x3 -> Trainer -> A ->
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
    `which` row -> A -> S1 info -> A -> box grid (cold rom_gbsprite scan) -> START ->
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
        s.tap("A", settle=gb_shots.BIG_SETTLE)                # ADD ITEM -> id entry (osk_search)
        s.shot("08_add_item_id", "U4: ADD ITEM asks for a raw item id (1-250) -- no "
                                  "item-name table this slice (time-boxed, see "
                                  "pdna_gbbag.h); the list itself already prints "
                                  "'ITEM-n' for the same reason ('-' not '#': "
                                  "'#' has no Gen-1 glyph, D9). Cancelling either "
                                  "prompt now aborts the whole ADD (num_entry_opt, D5) "
                                  "instead of silently inserting id 1 x1.")

        # osk_search's own key contract (source/osk.c): A INSERTS the on-screen
        # keyboard's currently-highlighted glyph (row 0 is the digit row "1234567890",
        # cursor starts at (0,0) == '1'), B is BACKSPACE, START confirms, SELECT
        # cancels -- NOT "A confirms" (an earlier version of this script got that
        # wrong and silently mistyped every field; caught by looking at shot 07,
        # which showed the bag list with a corrupted quantity instead of the item
        # menu). N4 (review): the ORIGINAL version of this demo used id 77 (GOOD
        # ROD), already at qty 99 in Red.sav -- but id 77 is a Gen-1 KEY item
        # (gbb_is_g1_key_item(), 4a1afc8), so its row prints NO quantity at all;
        # "09b shows the row reading x99" was never true of that capture. id 20
        # (POTION) is an ORDINARY item, not in Red.sav's pocket yet -- ADD ITEM it
        # straight in at qty 99 below (typed directly, the brief's "qty editor, or
        # an edge save" alternative is not needed since ADD ITEM's own num_entry
        # IS a qty editor), so the saturating merge right after lands on a row
        # that actually prints a quantity.
        s.tap("B", settle=gb_shots.SETTLE)                     # clear the seeded "1"
        s.press_n("RIGHT", 1, settle=gb_shots.SETTLE)          # keyboard cursor: col0 '1' -> col1 '2'
        s.tap("A", settle=gb_shots.SETTLE)                     # type '2' -> field "2"
        s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)          # col1 '2' -> col9 '0' (no B: '2' must stay typed)
        s.tap("A", settle=gb_shots.SETTLE)                     # type '0' -> field "20"
        s.tap("START", settle=gb_shots.BIG_SETTLE)             # confirm id=20 (POTION) -> quantity entry
        s.shot("08b_add_item_qty", "U4: then a quantity (1-99), the same num_entry -- "
                                    "id 20 (POTION, an ORDINARY item) typed via the "
                                    "digit row")
        s.tap("B", settle=gb_shots.SETTLE)                     # clear the seeded "1"
        s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)          # '1' -> '9' (row 0, col 8)
        s.tap("A", settle=gb_shots.SETTLE)                     # type '9' -> field "9"
        s.tap("A", settle=gb_shots.SETTLE)                     # type '9' again (cursor unmoved) -> field "99"
        s.tap("START", settle=gb_shots.BIG_SETTLE)             # confirm qty=99 -> gbb_insert(...,20,99): new entry
        s.shot("08c_potion_planted", "U4: id 20 (POTION) inserted fresh at qty 99 -- "
                                      "gbb_insert() takes the FREE-SLOT path (no "
                                      "existing id-20 entry to merge into), landing "
                                      "on the list's own last row; the qty editor "
                                      "step the N4 brief also allows (A on a row) is "
                                      "therefore not separately needed here -- ADD "
                                      "ITEM was typed straight to the cap")

        s.tap("START", settle=gb_shots.BIG_SETTLE)             # -> the item menu again, cursor still on POTION
        s.tap("A", settle=gb_shots.BIG_SETTLE)                 # ADD ITEM -> id entry again
        s.tap("B", settle=gb_shots.SETTLE)                     # clear seeded "1"
        s.press_n("RIGHT", 1, settle=gb_shots.SETTLE)          # col0 -> col1 '2'
        s.tap("A", settle=gb_shots.SETTLE)                     # type '2' -> field "2"
        s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)          # col1 -> col9 '0' (no B: keep the '2')
        s.tap("A", settle=gb_shots.SETTLE)                     # type '0' -> field "20" again
        s.tap("START", settle=gb_shots.BIG_SETTLE)             # confirm id=20 -> quantity entry
        s.tap("B", settle=gb_shots.SETTLE)                     # clear seeded "1"
        s.press_n("RIGHT", 4, settle=gb_shots.SETTLE)          # '1' -> '5' (row 0, col 4)
        s.tap("A", settle=gb_shots.SETTLE)                     # type '5'
        s.tap("START", settle=gb_shots.BIG_SETTLE)             # confirm qty=5 -> gbb_insert(...,20,5): MERGE path
        s.shot("09_saturation_refusal", "U4: merging qty 5 into id 20/POTION (already "
                                         "at the 99 cap from the ADD above) saturates "
                                         "and refuses -- gbb_insert() SETS the "
                                         "existing stack to the cap (99) and returns "
                                         "GBB_ERR_QTY (gb_bag.c's own 'sum > cap' "
                                         "branch WRITES list->entries[i].qty = "
                                         "GBB_QTY_CAP, it does not merely refuse); "
                                         "gbbag_start_menu's own msg_wait('SATURATED', "
                                         "...) reports it. The WRITE still happens "
                                         "here (99 -> 99) -- it is a no-op only "
                                         "because POTION was already at the cap; nothing "
                                         "about the mechanism itself skips the write.")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                 # dismiss the msg_wait -- back on the list
        # ADD ITEM's own `*sel = l->count - 1` leaves the cursor on the LAST entry --
        # POTION is that last entry (it was appended fresh above and nothing since
        # has added/removed a row), so it is already in frame with no scrolling.
        s.shot("09b_after_add", "U4: after dismissing the refusal, the cursor is "
                                 "already on POTION's own row (ADD ITEM's own "
                                 "'*sel = last entry' rule, unchanged since POTION "
                                 "was appended) -- it reads x99, confirming the "
                                 "saturating write landed exactly where it started "
                                 "(99 -> 99, see the 09 caption above); an ORDINARY "
                                 "item's row, unlike id 77/GOOD ROD (a Gen-1 KEY "
                                 "item, prints no quantity at all).")

        # The saturation refusal above made NO byte change (99 -> clamped-to-99 is a
        # true no-op), so B here would take the silent memcmp-no-op path, not the
        # commit prompt -- a REAL edit is needed first. REMOVE the currently selected
        # entry (POTION, left selected by the ADD ITEM path above) via the item menu.
        s.tap("START", settle=gb_shots.BIG_SETTLE)             # -> the item menu again
        s.tap("DOWN", settle=gb_shots.SETTLE)                  # ADD ITEM -> REMOVE
        s.tap("A", settle=gb_shots.BIG_SETTLE)                 # REMOVE the selected entry -- a REAL change
        s.shot("09c_removed", "U4: REMOVE deletes POTION -- a real, "
                               "persisted-if-confirmed change (unlike the saturation "
                               "attempt above)")

        s.tap("B", settle=gb_shots.BIG_SETTLE)                # B -> the commit prompt (a real edit pending)
        s.shot("10_commit_prompt", "U4: B with a real pending edit -> 'Save bag "
                                    "changes?' (app_confirm), the same dialog every "
                                    "other GB screen's own commit uses")
        s.tap("B", settle=gb_shots.BIG_SETTLE)                # decline -- discard the edit
        s.shot("11_declined", "U4: declining discards the edit -- gbb_write never ran, "
                               "back at the box grid")

        # N6(a): the armed-SWAP state (D10) -- START > SWAP on a row ARMS a mark,
        # it does not swap on the spot. Re-enter the bag screen (declining above
        # never persisted anything, so gbb_read() below re-reads the ORIGINAL
        # unedited pocket -- row 0/row 1 are back to their pristine ids).
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
                                       "Items list (the 09c/10/11 REMOVE was never "
                                       "written)")
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
        # arming here grabs row 1 (id 206, post-swap) as the source; move UP to
        # row 0 (id 205) as the destination, not DOWN, to swap the SAME pair back.
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

        # N6(a) part 2: B drops an armed mark WITHOUT moving anything -- the KEY_B
        # handler's own g1_swap_active branch never touches bag->pockets at all
        # (source/pdna_gbbag.c pdna_gbbag_gen1_screen, the `if (g1_swap_active) {
        # g1_swap_active = false; ...; continue; }` arm under KEY_B) -- a true
        # 0-byte change, provable from the code path itself: that branch contains
        # no assignment to any bag field, only the repaint. Demonstrated here by
        # what the emulator CAN show: leaving the screen right after affords no
        # confirm dialog at all, the same silent path 12c above takes for a real
        # no-op -- if B-drop-mark had mutated anything, `want_commit && memcmp(...)
        # != 0` would have popped 'Save bag changes?' instead.
        # 12c_swap_undone above already LEFT the bag screen (its own B fell
        # through to the box grid, the same "no-op path" 11_declined took) -- a
        # full re-entry is needed here, not just an item-menu re-open.
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
                                      "at all. That prompt only ever fires when "
                                      "memcmp(bag,t0)!=0 (pdna_gbbag() below "
                                      "pdna_gbbag_gen1_screen); its absence here IS "
                                      "the byte-compare proof for the B-cancel-mark "
                                      "path: 0 bytes changed, not merely 'looks "
                                      "unchanged on screen'.")

        # N6(b): BAD ID refusals -- ADD ITEM with id 0 and id 251 (Gen 1's own
        # range is 0x01..0xFA == 1..250, gb_bag.h's own VALID ITEM IDS comment;
        # 0 and 0xFB==251 are both one step outside either edge).
        s.run(250)                                              # re-entry idle (see the D-reentry note above)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> nav menu
        s.press_n("DOWN", 7, settle=gb_shots.SETTLE)
        s.tap("A", settle=GB_ART_COLD_SETTLE)                   # back into the bag screen
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> item menu (csel 0 == ADD ITEM)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry, seeded "1"
        s.tap("B", settle=gb_shots.SETTLE)                      # clear seeded "1" -> field empty
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm id="" -- num_entry_opt treats an
                                                                 # empty field as 0, the refused id below --
                                                                 # -> QUANTITY prompt next (seeded "1", already
                                                                 # valid; gbb_insert() only runs after BOTH
                                                                 # prompts confirm, so id alone shows nothing yet)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=1 (seeded, unedited) -> NOW
                                                                 # gbb_insert(id=0, qty=1) actually runs
        s.shot("14_bad_id_zero", "U4: N6(b) -- ADD ITEM with id 0 (the field left "
                                  "empty, which num_entry_opt reads back as 0) -> "
                                  "gbb_insert() returns GBB_ERR_BADID -- "
                                  "msg_wait('BAD ID', ..., 'That item id does not "
                                  "exist.')")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss

        # N6(b) part 2 -- id 251: one past Gen 1's last legal id (250). R1 (the
        # re-verify-3 one-liner): the ID prompt's OSK cap is 999 and the value is
        # clamped to 0xFF, so 251 reaches gbb_insert() as 251 and lands on its own
        # GBB_ERR_BADID branch -> the same 'BAD ID' dialog as id 0. (Before R1 the
        # prompt clamped to 250 = TM50 and INSERTED it silently.)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # item menu again
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry, seeded "1", cursor col0
        s.tap("B", settle=gb_shots.SETTLE)                      # clear seeded "1" -> field empty, cursor col0
        s.press_n("RIGHT", 1, settle=gb_shots.SETTLE)           # col0 -> col1 '2'
        s.tap("A", settle=gb_shots.SETTLE)                      # type '2' -> field "2"
        s.press_n("RIGHT", 3, settle=gb_shots.SETTLE)           # col1 -> col4 '5'
        s.tap("A", settle=gb_shots.SETTLE)                      # type '5' -> field "25"
        s.press_n("RIGHT", 6, settle=gb_shots.SETTLE)           # col4 -> col10 mod 10 == col0 '1' (osk.c's
                                                                 # own KEY_RIGHT wraps `(cc + 1) % rowlen`,
                                                                 # source/osk.c line 223 -- the digit row is
                                                                 # 10-wide, so RIGHT wraps circularly)
        s.tap("A", settle=gb_shots.SETTLE)                      # type '1' -> field "251"
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm id "251" -> QUANTITY prompt (seeded "1")
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=1 -> gbb_insert(id=251) -> GBB_ERR_BADID
        s.shot("15_id251_bad_id", "U4: N6(b) -- typing id 251 (one past Gen 1's last "
                                   "legal id) now reaches gbb_insert() unchanged and is "
                                   "refused with the same 'BAD ID / That item id does not "
                                   "exist.' dialog as id 0 -- this frame is pixel-identical "
                                   "to shot 14 BY DESIGN (the dialog never echoes the typed "
                                   "id; allow_same) -- R1: the ID prompt no longer clamps "
                                   "to 250; nothing was inserted.", allow_same=True)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss; nothing to undo

        # N6(c): BAD QUANTITY refusals -- qty 0 and qty 100 (valid range 1..99).
        # A valid id is needed to reach the quantity prompt at all; id 20 (POTION,
        # not currently in the pocket, same id N4 used) keeps this an INSERT, not
        # a merge, so the refusal is unambiguously about the typed quantity.
        s.tap("START", settle=gb_shots.BIG_SETTLE)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry
        s.tap("B", settle=gb_shots.SETTLE)
        s.press_n("RIGHT", 1, settle=gb_shots.SETTLE)           # -> '2'
        s.tap("A", settle=gb_shots.SETTLE)
        s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)           # -> '0'
        s.tap("A", settle=gb_shots.SETTLE)                      # id "20"
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> quantity entry
        s.tap("B", settle=gb_shots.SETTLE)                      # clear seeded "1" -> empty (reads back as 0)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=0
        s.shot("16_bad_qty_zero", "U4: N6(c) -- qty 0 (the field left empty) -> "
                                   "gbb_insert() returns GBB_ERR_QTY, and because "
                                   "the TYPED value (0) was itself outside 1..99 "
                                   "(qty_in_range false), gbbag_start_menu's own "
                                   "branch reports 'BAD QUANTITY' / 'Quantity must "
                                   "be 1-99.' -- NOT 'SATURATED' (that wording is "
                                   "reserved for a legal typed value that overflowed "
                                   "an existing stack on merge, see the N4 shots)")
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss
        s.run(80)                                               # extra margin -- this stretch flaked during
                                                                 # authoring under BIG_SETTLE alone (mGBA
                                                                 # timing, the same class the 04/blink-marker
                                                                 # caption already documents)

        s.tap("START", settle=gb_shots.BIG_SETTLE)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry
        s.tap("B", settle=gb_shots.SETTLE)
        s.press_n("RIGHT", 1, settle=gb_shots.SETTLE)
        s.tap("A", settle=gb_shots.SETTLE)
        s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # id "20" again
        s.run(80)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> quantity entry, seeded "1", cursor col0
        s.tap("B", settle=gb_shots.SETTLE)                      # clear seeded "1" -> field empty, cursor col0
        s.tap("A", settle=gb_shots.SETTLE)                      # col0 IS '1' -- type it directly -> "1"
        s.press_n("RIGHT", 9, settle=gb_shots.SETTLE)           # col0 -> col9 '0'
        s.tap("A", settle=gb_shots.SETTLE)                      # type '0' -> "10"
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # cursor unmoved (still col9 '0') -> "100"
        s.shot("17_bad_qty_100_typed", "U4: N6(c) -- '100' typed into the SAME "
                                        "quantity prompt as 16 (one past the 99 "
                                        "cap, not clamped by the OSK -- num_entry_"
                                        "opt's own maxv for THIS prompt is 999, "
                                        "not 99, exactly so a typed 100 reaches "
                                        "gbb_insert()'s own validation instead of "
                                        "being silently clamped first, D5)")
        s.run(80)
        s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=100 -> gbb_insert() returns
                                                                 # GBB_ERR_QTY, qty_in_range false again -- the
                                                                 # SAME 'BAD QUANTITY' / 'Quantity must be
                                                                 # 1-99.' dialog as 16 (msg_wait's own text
                                                                 # never echoes the typed value, so the two
                                                                 # dialogs are PIXEL-IDENTICAL -- not
                                                                 # re-captured here on purpose: gb_shots.py's
                                                                 # own Session.shot() refuses a pixel-identical
                                                                 # repeat as a likely driver bug, and here it
                                                                 # would be right to be suspicious of a NEW
                                                                 # bug except this one really is the same
                                                                 # dialog by design; 17_bad_qty_100_typed above
                                                                 # is the honest proof of what was actually
                                                                 # typed instead)
        s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss (msg_wait's own "Press A")
        s.tap("B", settle=gb_shots.BIG_SETTLE)                  # leave -- every refusal above made no edit,
                                                                 # so this is the silent no-op path again
        s.shot("18_left_after_refusals", "U4: leaving after every N6(b)/(c) refusal "
                                          "-- no confirm dialog, same no-op-path "
                                          "proof as 13c above: none of the BAD ID / "
                                          "BAD QUANTITY attempts wrote anything")

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
    instead of skipping straight to the GB save's S1 info page).

    Nav: single-ROM image, so `gb_delta_pick_save()`'s `if (n == 1) return 0`
    skips the boot picker entirely -- ONE tap (A: S1 info -> box grid), not
    DOWN+A+A the way run_m1_map()'s combined multi-ROM image needs. From the
    box grid: START -> nav menu -> DOWN x16 (same PDNA_NAV_ITEMS index as
    Gen 1's Map row -- the list order does not change per generation) -> A ->
    pdna_gbmap_gen2().

    Crystal.sav's real player position (group 24 / number 4 = NEW_BARK_TOWN,
    y=6/x=13 -> block (6,3) via gbmap_block_of()'s coord>>1) sits on a 10x9-
    block map: vbx starts at clampi(6-5//2, 0, 10-5=5) = 4, vby at
    clampi(3-5//2, 0, 9-5=4) = 1 -- the marker lands exactly centred on open
    ground (design doc §2.4/§8's own cross-checked New Bark Town dump)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "m1_map_g2_")
    print("== M1-G2: Crystal's own current-map view (single-ROM -> Map) ==")

    s.run(700)
    s.tap("A", settle=60)                                    # S1 info -> box grid (single-ROM image)
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
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "m1_map_g2_wronggame_")
    print("== M1-G2: wrong-game refusal (Gold save, Crystal-only ROM) ==")

    s.run(700)
    s.tap("A", settle=60)
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
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "m1_map_g2_norom_")
    print("== M1-G2: no-ROM refusal (Gold save, no Gen-2 ROM fused) ==")

    s.run(700)
    s.tap("A", settle=60)
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
    own doc comment) -- one A tap reaches S1 info -> box grid directly.

    Nav: A (S1 info) -> box grid (rom_gbsprite cold scan) -> START -> nav menu
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

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (TM/HM: PC STORE/CANCEL only,
                                                              # csel starts on PC STORE, no DOWN needed)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # toggle -> PC store
    s.shot("05_pc_store", "U5: START > PC STORE toggles to the PC item store -- "
                           "the nameplate label under the picture still reads "
                           "'Items' (reused, not overridden; not independently "
                           "pixel-dumped this slice, see pdna_gbpack.h). D6 "
                           "fix: the description box is the ONE thing that "
                           "changes, now printing 'PC ITEM STORE' -- the "
                           "earlier draft's empty box left the store visually "
                           "identical to the Items pocket")

    # D7 fix (review-opus ac9ffc0): ADD ITEM from the PC store is no longer
    # refused -- it is its own undifferentiated list, not one of the four
    # real bag pockets the Items-only fallback rule was meant to guard. This
    # save's PC store is already at capacity (BAG FULL on a completed add is
    # a real, expected refusal -- a full pocket, not a wrong one) so the demo
    # only needs to show the id-ENTRY screen opening (proof ADD ITEM is no
    # longer refused outright), then cancel out with SELECT (osk_search's own
    # cancel, same as shot 08's qty editor) rather than complete the insert --
    # osk_search's `continue` lands back in the SAME open PACK MENU (csel
    # still 0), so no extra START tap is needed before the DOWN x3 below.
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (PC store, csel=0=ADD ITEM)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry (seeded "1")
    s.shot("05b_pc_store_add_item", "U5 D7 fix: START > ADD ITEM from the PC "
                                     "ITEM STORE opens the id entry instead of "
                                     "refusing 'WRONG POCKET' (the earlier "
                                     "draft's behaviour) -- this save's PC "
                                     "store happens to already be full, so "
                                     "this demo cancels out rather than "
                                     "complete an insert that would correctly "
                                     "show BAG FULL, a different, expected "
                                     "refusal")
    s.tap("SEL", settle=gb_shots.BIG_SETTLE)                # osk_search: SELECT cancels -> back in the menu

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

    s.tap("RIGHT", settle=gb_shots.BIG_SETTLE)              # Items -> Balls
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (ADD ITEM/REMOVE/SWAP/PC STORE/CANCEL)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM on Balls -> WRONG POCKET refusal
    s.shot("09_wrong_pocket_refusal", "U5: START > ADD ITEM from the BALLS "
                                       "pocket refuses outright -- 'WRONG "
                                       "POCKET' / 'Add items from the Items "
                                       "pocket.' / 'No per-item pocket table "
                                       "yet.' (the dim third line; per-item "
                                       "pocket membership was not located "
                                       "this slice; the brief's own "
                                       "sanctioned fallback is Items-only "
                                       "ADD ITEM, not a silent wrong-pocket "
                                       "accept)")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -> back in the PACK MENU (msg_wait's
                                                              # own `continue` loops the menu, does NOT
                                                              # close it -- gbpack_start_menu's own ADD ITEM
                                                              # branch, csel unchanged at 0)
    s.press_n("DOWN", 4, settle=gb_shots.SETTLE)            # ADD ITEM -> REMOVE -> SWAP -> PC STORE -> CANCEL
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # CANCEL -> back to the list (still Balls)
    s.tap("LEFT", settle=gb_shots.BIG_SETTLE)               # Balls -> Items

    # Saturation refusal on an ORDINARY item: ADD ITEM the currently-selected
    # pocket's own id 1 (not present yet) at qty 99 (a SILENT success -- no
    # message, gbb_insert() returns GBB_OK -- so the menu loop's own `continue`
    # lands right back on ADD ITEM with NO extra tap needed), then ADD ITEM id
    # 1 again at qty 5, which MERGES into the fresh 99 stack and overflows the
    # cap -- THIS one does show SATURATED. osk_search's own contract: row 0 is
    # the digit row "1234567890", cursor starts at (0,0) == '1', A inserts the
    # highlighted glyph, B backspaces, START confirms, SELECT cancels (U4's
    # own precedent, run_u4_bag() above).
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (Items, csel=0=ADD ITEM)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM -> id entry (seeded "1")
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm id=1 -> quantity entry
    s.tap("B", settle=gb_shots.SETTLE)                      # clear seeded "1"
    s.press_n("RIGHT", 8, settle=gb_shots.SETTLE)           # col0 -> col8 '9'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '9'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '9' again -> field "99"
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=99 -> gbb_insert(id=1,99): FREE-SLOT
                                                              # path, GBB_OK, NO message -- gbpack_start_menu's
                                                              # own ADD ITEM branch falls through to `return 0`
                                                              # unconditionally after a COMPLETED add (success
                                                              # OR an error message dismissed), closing the
                                                              # menu straight back to the LIST -- only the
                                                              # WRONG-POCKET early-refuse `continue`s and stays
                                                              # in the menu; this is NOT that case.
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU again (fresh open, csel=0=ADD ITEM)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # ADD ITEM AGAIN -> id entry (seeded "1")
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm id=1 -> quantity entry
    s.tap("B", settle=gb_shots.SETTLE)
    s.press_n("RIGHT", 4, settle=gb_shots.SETTLE)           # col0 -> col4 '5'
    s.tap("A", settle=gb_shots.SETTLE)                      # type '5'
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # confirm qty=5 -> merge overflows 99 -> SATURATED
    s.shot("10_saturation_refusal", "U5: re-adding id 1 at qty 5 merges into "
                                     "the existing (already-99) stack -- "
                                     "gbb_insert() saturates it at the cap and "
                                     "reports SATURATED (same mechanism as "
                                     "U4's own N6(c))")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss -> gbpack_start_menu's own ADD ITEM
                                                              # branch falls through to `return 0` after this
                                                              # (a COMPLETED add, message or not) -- back at
                                                              # the LIST, not the menu.

    # SWAP: arm row 0, move down, confirm the destination. The cursor is
    # currently on the LAST real row (the fresh id-1 insert, ADD ITEM's own
    # `*sel = l->count - 1` rule) -- move it UP first so SWAP arms a row with
    # a REAL row below it, not the trailing CANCEL row (arming the last real
    # row and pressing DOWN would land on CANCEL, which the destination guard
    # correctly refuses -- sel < cnt is false for it -- but that is a
    # different, less illustrative demo than an actual two-item swap).
    s.press_n("UP", 2, settle=gb_shots.SETTLE)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # -> PACK MENU (fresh open, csel=0=ADD ITEM)
    s.press_n("DOWN", 2, settle=gb_shots.SETTLE)            # ADD ITEM -> REMOVE -> SWAP
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # SWAP: arms row 0, returns to the list
    s.tap("DOWN", settle=gb_shots.SETTLE)                   # cursor off the source row
    s.shot("11_swap_armed", "U5: SWAP arms row 0 (the mark stays lit there) "
                             "and returns to the list -- pick-source-then-"
                             "destination, same semantic as U4's own Gen-1 "
                             "SWAP")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # A on the destination -> the actual swap
    s.shot("11b_swap_done", "U5: A on the destination performs the swap -- "
                             "both rows traded places, the mark is gone")
    # Arm again and drop with B instead of swapping.
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 2, settle=gb_shots.SETTLE)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # re-arm on the current row
    s.tap("DOWN", settle=gb_shots.SETTLE)
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B drops the mark, nothing moves
    s.shot("11c_swap_dropped", "U5: B drops an armed SWAP mark without leaving "
                                "the screen or moving anything -- the row's own "
                                "0xEC armed-swap marker is gone (0xED is the "
                                "plain cursor, a different glyph), no entries "
                                "changed")

    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # B with a real pending edit -> commit prompt
    s.shot("12_commit_prompt", "U5: B with a real pending edit -> 'Save pack "
                                "changes?' (app_confirm), the same dialog "
                                "every other GB screen's own commit uses")
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # decline
    s.shot("13_declined", "U5: declining discards the edit -- gbb_write never "
                           "ran, back at the box grid")

    return s


def run_b89_hof(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #89: the Gen-1/2 Hall of Fame screen (source/pdna_gbhof.c) over
    gb_hof.h's core -- the same single-ROM-image / nav-menu-DOWN shape run_b90_fly()
    above uses, reused for a plain list->detail screen. `rom` must be a ONE-ROM
    fused image (Red-only for `which == "red"`, Crystal-only for `which ==
    "crystal"`, same BACKLOG #98 harness-gap reasoning as U4/U5/b90's own images).

    Nav: A (S1 info) -> box grid -> START -> nav menu -> DOWN x12 (Party=0, Bank=1,
    Daycare=2, Trainer=3, Clock fix=4, Mirage=5, Pokedex=6, Bag=7, Flags&counters=8,
    Bases=9, Blocks=10, Tickets=11, Records=12 -- PDNA_NAV_ITEMS order,
    source/pdna_layout.h) -> A -> pdna_gbhof()."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b89_{which}_")
    print(f"== BACKLOG #89: {which}'s own Hall of Fame (single-ROM image -> "
          "standalone -> Records) ==")

    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)              # box grid -> nav menu
    s.press_n("DOWN", 12)                                    # Party -> ... -> Records (index 12)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Records -> pdna_gbhof()
    s.shot("01_list", f"BACKLOG #89: {which}'s own Hall of Fame list -- the "
                       "'N teams (lifetime count C)' header, one row per recorded "
                       "team newest-first, cursor on row 1")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # row 1 -> the team detail
    # D6: this corpus save's own HoF teams carry no shiny DV quad and no custom
    # nickname (real, unedited saves) -- do not claim either in THIS shot's own
    # caption. Both are proven on dedicated poked-.sav shots kept alongside this
    # set (b89_{red,crystal}_08_nick.png, b89_crystal_09_shiny.png), not implied here.
    s.shot("02_detail", "BACKLOG #89: the team detail page -- 6 mon rows"
                        + (" (species/level, OT id)" if which == "crystal" else " (species/level)"))
    s.tap("B", settle=gb_shots.BIG_SETTLE)                  # detail -> back to the list

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # the Hall of Fame's own START menu
    s.shot("03_menu", "BACKLOG #89: START -> CLEAR ALL / SET COUNT")

    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # CLEAR ALL -> app_confirm
    s.shot("04_clear_confirm", "BACKLOG #89: CLEAR ALL -> the real consequence -- "
                                "\"The PC's HALL OF FAME option disappears until "
                                "you win again.\" (app_confirm)")
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
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # dismiss gb_persist's own dialog -> back to the list
    s.shot("06_empty", "BACKLOG #89: after CLEAR ALL -- '0 teams (lifetime count "
                        "0)', 'No teams recorded yet.'")

    s.tap("START", settle=gb_shots.BIG_SETTLE)              # menu again, on the now-empty list
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


def run_b89_hof_detail_only(core_mod, image_mod, rom: Path, out_dir: Path, which: str,
                            shot_name: str, caption: str) -> gb_shots.Session:
    """BACKLOG #89 D6/NICK proof shots: navigate straight to the team detail page
    and shoot it once, nothing else. `rom` must be the SAME kind of ONE-ROM fused
    image run_b89_hof() takes, except its embedded .sav has been byte-poked first
    -- see this slice's own commit message for the exact offsets poked and why
    re-fusing the poked .sav (not poking the already-fused .gba's SAV payload
    directly) is required: fuse_gb.py's directory records a CRC32 per payload, and
    poking the fused output invalidates it, which reads back as 'not a valid save'
    at boot.

    which=red/crystal + kind=nick: mon 0 nicknamed (gb_name_encode).
    which=crystal + kind=shiny (R2 re-verify): mon 0 poked BOTH nicknamed AND
    shiny-capable (g2_dv_shiny's Atk&2/Def=Spe=Spc=10 quad) at Lv 100 -- the exact
    worst-case suffix (" Lv.100 *", 9 chars + NUL) that overflowed the old
    char[8] (R2); a shiny-only fixture with no nickname, as the first fix pass
    shipped, never exercised that branch at all."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b89_{which}_")
    print(f"== BACKLOG #89 D6/NICK: {which}'s Hall of Fame detail, poked-.sav proof shot ==")
    boot_to_gb_session(s, rom, which=which)
    s.tap("START", settle=gb_shots.BIG_SETTLE)
    s.press_n("DOWN", 12)
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # Records -> pdna_gbhof()
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # row 1 -> the team detail
    s.shot(shot_name, caption)
    return s


def run_b90_fly(core_mod, image_mod, rom: Path, out_dir: Path, which: str) -> gb_shots.Session:
    """BACKLOG #90: the Gen-1/2 Fly-destination screen (source/pdna_gbfly.c) over
    gb_fly.h's bitfield core -- the same shape as run_u4_bag()/run_u5_pack() above,
    reused for a plain (non-gbscreen-shell) list screen. `rom` must be a ONE-ROM
    fused image (Red-only for `which == "red"`, Crystal-only for `which ==
    "crystal"` -- same BACKLOG #98 harness-gap reasoning as U4/U5's own images), so
    the boot picker is skipped (gb_delta_pick_save()'s `if (n == 1) return 0`) and
    one A tap reaches S1 info -> box grid directly.

    Nav: A (S1 info) -> box grid -> START -> nav menu -> DOWN x14 (Party=0, Bank=1,
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

    Nav: A (S1 info) -> box grid (cur=0, on_title=false) -> UP (on_title=true, the
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
    NV_GB import mount (nav_to_gb_import(), reached from a combined image's default
    Emerald row, same navigation run_nav_gb() already uses). `rom` must be the
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

    # Mount 2: the nested START > NV_GB import mount -- default Emerald row (0) on
    # the SAME combined image -> ordinary Gen-3 box screen -> nav_to_gb_import() ->
    # `which`'s own row on the (separate) nested-import picker -> that save's own
    # S1 info page -> box grid (cold-fetched, same posture as run_nav_gb()).
    idx = gb_save_pick_index(rom)[which]
    s2 = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b132_nested_{which}_")
    print(f"== BACKLOG #132: {which}'s own summary portrait, nested NV_GB import mount ==")
    s2.run(700)
    s2.tap("A", settle=gb_shots.BIG_SETTLE)                   # top-level picker, Emerald row (default) -> box
    nav_to_gb_import(s2)                                        # box screen -> nav menu -> NV_GB -> the save picker
    for _ in range(idx):
        s2.tap("DOWN", settle=gb_shots.SETTLE)
    s2.tap("A", settle=gb_shots.BIG_SETTLE)                   # picked -> this save's own S1 info page
    s2.tap("A", settle=60)                                     # info -> box grid (COLD fetch starts)
    s2.run(GB_ART_COLD_SETTLE)
    s2.shot("01_box_grid", f"BACKLOG #132: {which}.sav's box grid via the nested "
                            "NV_GB import mount -- box 1, cursor on slot 1 (same "
                            "hover-panel control as the boot-picker mount above)")
    s2.tap("A", settle=gb_shots.BIG_SETTLE)                   # A on slot 1 -> its menu
    s2.shot("02_mon_menu", f"BACKLOG #132: {which}.sav -- the nested-import mount's "
                            "own occupied-cell menu: VIEW / LEGALITY / COPY / "
                            "CANCEL only (no EDIT/MOVE TO BOX/RELEASE)")
    s2.tap("A", settle=gb_shots.BIG_SETTLE)                   # VIEW -> the native summary (read-only)
    s2.shot("03_view_summary", f"BACKLOG #132: {which}.sav -- VIEW via the nested "
                                "import mount: same expectation as the boot-picker "
                                "mount's summary shot above (Gold changes to its "
                                "own GB sprite, Red stays pixel-identical) -- both "
                                "mounts reach the SAME pdna_origin_art_portrait(), "
                                "so this is the second, independent proof.")
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

    Nav: S1 info -> A -> box grid (rom_gbsprite cold scan) -> R x(box index) ->
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
    same gate) skips straight to the GB save's S1 info page with NO boot-picker
    taps at all -- unlike run_u4_bag()'s own doc comment above (which describes
    the combined multi-ROM image's picker), this image needs exactly ONE tap
    (A: S1 info -> box grid), not DOWN+A+A. Then: START -> nav DOWN x7
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
    and lands straight on the GB save's S1 info page, so this needs exactly ONE tap
    (A: S1 info -> box grid), not DOWN+A+A the way the combined multi-ROM image does.

    Captures: the Clock screen itself (offsets/day-count/weekday/flag readout + the
    three rows), each row's own app_confirm (Ask/Shift/Clear), the shift row's signed
    +-days/hours/minutes editor, and the shift confirm's own dynamic delta line -- B
    declines every confirm except the shift row's (which is driven through to
    gb_persist()'s PDNA_DELTA in-session-only refusal, the same honest outcome every
    other delta-build write ends at, so the shots prove the write PATH runs, not that
    it lands on a card that does not exist in this build)."""
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

    s.tap("L", settle=gb_shots.BIG_SETTLE)                   # DV_LIST(1) -> DV_GRID(0)
    s.shot("04_grid", "#87: L once -> DV_GRID -- the icon grid (art-free build: name "
                       "chips, per dex_cell_grid's own art-free fallback), same cap")

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
    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # dex_menu -> B closes it, back to the list
    s.shot("12_after_undo", "#87: back at the list after Undo -- restored to exactly the "
                             "pre-Catch-ALL state (dex_bulk's s_dex_snap[386], "
                             "byte-exact per the acceptance gate) -- compare against "
                             "03_list_default")

    if which == "crystal":
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

    s.tap("B", settle=gb_shots.BIG_SETTLE)                   # chooser (or dex screen, Red) -> pdna_gbdex()'s own confirm
    s.shot("16_save_confirm", "#87: 'Save Pokedex changes?' -- item 3's own end-of-visit "
                               "confirm (gbs_finish + gb_persist('dex') on A)")
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
    tag = f"b124_dexicons_{which}{'_fallback' if fallback else ''}_"
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, tag)
    print(f"== BACKLOG #124: dex-grid GB icons ({which}{' fallback' if fallback else ''}) ==")
    s.run(700)
    s.tap("A", settle=60)                                   # S1 info -> box grid (single-ROM image)
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
    s.tap("L", settle=gb_shots.BIG_SETTLE)
    s.shot("02_dex_grid", "#124: DV_GRID page 1 -- " +
           ("GB ROM icons via gbdex_cell_art() for every seen/caught species (this "
            "backlog's own render)" if (which == "crystal" and not fallback) else
            "Gen 1: unchanged icon-store/mon_icon_for grid (gb_art_source.c's own "
            "rule -- Gen 1 has no menu icons, gbdex_cell_art() self-gates on "
            "s->gen != GB_GEN2)" if which == "red" else
            "no GB ROM registered (fallback image) -- pdna_origin_art_have(PDNA_GEN2) "
            "refuses, dex_cell_grid() falls straight back to the unchanged icon-store "
            "ladder, byte-identical to a pre-#124 build"))
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


def measure_dex_sd_reads(core_mod, image_mod, rom: Path, out_dir: Path) -> None:
    """BACKLOG #124 step 2: the 'perf dex:' log line's own `sd Xr/...` field for ONE
    Crystal dex-grid page (run_b124_dexicons's exact navigation, GB ROM registered,
    no fallback) against the SAME image's box-grid 'perf box:'/'perf bank:' line for
    a comparable single-page paint -- both spans already exist in this tree
    (pdna_pick.c's pdna_dex_screen() and pdna_box.c's own span), so this reuses them
    rather than adding new instrumentation. Installs a capturing logger (same
    mechanism as tools/perf_parity.py's load_mgba_capturing) instead of taking
    screenshots, and just prints every 'perf ' line captured during the run -- read
    the LAST 'perf dex: ... sd Nr/...' and 'perf box:'/'perf bank: ... sd Nr/...'
    lines by hand off stdout (no parser here: this is a one-off measurement, not a
    gate this tool enforces)."""
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
    s = run_b124_dexicons(core_mod, image_mod, rom, out_dir, "crystal", fallback=False)
    s.run(150)   # let perf's rate-limited flush land (perf_parity.py's own FLUSH_SETTLE)
    print("== BACKLOG #124 step 2: captured perf lines (read 'perf dex:'/'perf box:'"
          "/'perf bank:' sd Nr by hand) ==")
    for l in lines:
        if l.startswith("perf "):
            print(f"  {l}")


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
    S1 info -> A -> box grid, same shape as run_u4_bag()'s own boot-picker
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
    s.tap("A", settle=60)
    s.tap("A", settle=300)
    return _crop_bytes(s.screen)


def _measure_box_grid_cold_start(core_mod, image_mod, rom: Path,
                                 checkerboard_ref: bytes) -> tuple[int, bytes]:
    """Boots `rom`, drives the boot picker down to Red.sav (row 1), picks it, opens the
    S1 info page, then starts a frame-accurate timer at the exact frame the box grid
    is REQUESTED (right after the key-up edge of the second A press) and polls
    _PORTRAIT_CROP every _SAMPLE_EVERY frames until it stops matching EITHER of two
    "not real art yet" references: the S1 info page's own crop (still mid-transition)
    and `checkerboard_ref` (the "loading" placeholder) -- whichever a given build
    actually shows on its way to the real portrait, this is the first frame that is
    neither. Returns (frames_to_first_paint, final_screen_rgb_bytes) -- the second
    value is for a caller to diff the LOC-seeded and scanned paths' full final frames
    against each other (they must render the identical picture, portrait included)."""
    s = gb_shots.Session(core_mod, image_mod, rom, Path("/tmp"), "measure_")
    s.run(700)
    s.tap("DOWN", settle=gb_shots.SETTLE)   # boot picker: Emerald row (0) -> Red row (1)
    s.tap("A", settle=60)                   # pick Red -> S1 info page
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
        s.core.run_frame()
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
    ok: list[tuple[str, str]] = []
    from PIL import Image
    for label, px, frames in (("loc", loc_px, loc_frames), ("noloc", noloc_px, noloc_frames)):
        name = f"dgb_coldstart_{label}.png"
        path = out_dir / name
        Image.frombytes("RGB", (240, 160), px).save(path)
        caption = (f"#68b: Red.sav box grid, cold start {'WITH' if label == 'loc' else 'WITHOUT'} "
                   f"the fused rom_gb*_open_loc() record -- {frames} frames "
                   f"({frames / GBA_FPS:.2f} s emulated) to first stable paint")
        ok.append((name, caption))
        print(f"  [ok]   {name:32s} {caption}")
    return ok, []


def _write_manifest(out_dir: Path, ok: list[tuple[str, str]], skipped: list[tuple[str, str]]) -> None:
    """Same merge-by-file/merge-by-name block tools/gb_shots.py's own main() uses
    (BACKLOG #62 review D3: this script never wrote one at all before). Merged, not
    overwritten, for the identical reason: a partial run must not erase captions a
    separate prior run already wrote."""
    manifest_path = out_dir / "manifest.json"
    existing = {"shots": [], "skipped": []}
    if manifest_path.is_file():
        existing = json.loads(manifest_path.read_text(encoding="utf-8"))
    by_file = {e["file"]: e for e in existing.get("shots", [])}
    for n, c in ok:
        by_file[n] = {"file": n, "caption": c}
    by_name = {e["name"]: e for e in existing.get("skipped", [])}
    for n, r in skipped:
        by_name[n] = {"name": n, "reason": r}
    manifest_path.write_text(
        json.dumps({"shots": list(by_file.values()), "skipped": list(by_name.values())}, indent=2),
        encoding="utf-8")


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
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"s2bank_{which}_")
    print(f"== #120: the Bank from a Game Boy session ({which}) ==")
    s.run(700)
    s.tap("A", settle=60)                                   # S1 info -> box grid (single-ROM image)
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
        cs.tap("A", settle=60)                              # single-slot image: info -> box grid directly
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
    s.tap("A", settle=150)                                  # -> pdna_bank_show()
    s.shot("02_bank", "#120 F1 control: the real Bank, from an ordinary Gen-3 session")
    s.tap("A", settle=150)                                  # A on the empty cell -> its menu
    s.shot("03_empty_menu_create_and_paste", "#120 F1 control: the empty Bank "
           "cell's menu -- CREATE, PASTE HERE, CANCEL -- BOTH present, unchanged by "
           "the F1 gate (xg_create_row/xg_paste_row are true throughout an ordinary "
           "Gen-3 session)")
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


def run_s150_2_bank_native(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-2: a native ("GBC1") Bank cell renders as the Gen-1/2 Pokemon
    it is. `rom` MUST be `tools/fuse_sav.py <pokedna-delta-artless.gba> <Emerald.sav>`
    (a plain Gen-3 fusion, no --gb, no --clip -- same vehicle as run_s2_bank_control) --
    NO fused payload is needed because the cells themselves come from source/
    bank_plant.c's PDNA_DELTA-only hook in box_load() (source/pdna_bank.c): on this
    build there is never a real box file to read (no SD card at all in a delta image),
    so box 0 plants bank_plant_box0()'s five directed cells and box 1 plants
    bank_plant_box_full()'s 30-NATIVE worst case, automatically, the first time either
    is paged in.

    Same boot/nav recipe as run_s2_bank_control's own docstring: START, DOWN, A ->
    pdna_bank_show(), landing on box 0 (BANK 1 in the on-screen banner) -- ALREADY the
    planted grid, no extra navigation needed to reach it. mGBA timing is NOT the
    measurement the S150-2 acceptance row asks for (a real hardware box-flip timing for
    the 30-native box) -- this panel is a RENDER proof only; the perf numbers come from
    real hardware, reported separately."""
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
    s.shot("00_planted_box0", "S150-2: BANK 1, box_load()'s PDNA_DELTA plant -- five "
           "native cells at slots 0-4 (CHIKORITA/2, PIKACHU/1, an Egg, an item "
           "holder, the DMG chip), the rest of the grid ordinary empty Gen-3 slots -- "
           "no '?' badge anywhere; every native cell wears its era mark EXCEPT the "
           "DMG cell (species 252 + isBadEgg, D-Q3 -- review F3: no era to claim)")

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
           "the native WHITELIST (VIEW/MOVE/RELEASE/CANCEL)")
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
           "this is the native WHITELIST (VIEW/MOVE/RELEASE/CANCEL)")
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


def run_s150_3_escape_gate(core_mod, image_mod, rom: Path, out_dir: Path) -> gb_shots.Session:
    """BACKLOG #150 S150-3: the escape-route gate -- the mon-menu whitelist on a native
    Bank cell (VIEW/MOVE/RELEASE/CANCEL only), the deny toast when a native cell in hand
    reaches the drop_held gate (dragged out of the Bank into the PC), and the real
    Gen-1/2 summary opened from the whitelist's own VIEW row. Reuses S150-2's exact
    fixture and nav recipe (same box0 plant, same --image requirements: a plain
    tools/fuse_sav.py fusion of an Emerald.sav onto pokedna-delta-artless.gba, no --gb,
    no --clip) -- see run_s150_2_bank_native()'s own docstring for why no fused payload
    is needed (source/bank_plant.c's PDNA_DELTA-only box_load() hook)."""
    s = gb_shots.Session(core_mod, image_mod, rom, out_dir, "s150_3_")
    print("== BACKLOG #150 S150-3: the escape-route gate ==")
    s.tap("START", settle=80)                              # nav menu
    s.tap("DOWN", settle=60)                                # Party -> Bank (index 1, one DOWN)
    s.tap("A", settle=150)                                  # -> pdna_bank_show(), box 0 (BANK 1)
    # cursor already on slot 0 (the CHIKORITA plant). A opens the mon menu.
    s.tap("A", settle=150)                                  # slot 0 -> its menu
    s.shot("00_native_menu", "S150-3: A on the CHIKORITA native cell -- decision 7's "
           "WHITELIST, not the ordinary eleven-row occupied menu: VIEW / MOVE / "
           "RELEASE / CANCEL only, cursor defaults to row 0 (VIEW)")

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
    s.tap("DOWN", settle=60)                                # row 0 (VIEW) -> row 1 (MOVE)
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
    # Build a real (non-native) mon at the first empty slot (slot 5: box0's plant only
    # populates 0-4), give it an item via the ordinary ITEM menu row, then TAKE it in
    # the grid's own ITEM cursor mode and try to GIVE/swap it onto the native CHIKORITA
    # cell at slot 0 -- box_set_held()'s bc_is_native() refusal must fire BEFORE any
    # write, with the native cell's own data untouched and the item still in hand.
    s.press_n("RIGHT", 5, settle=150)                       # slot 0 -> slot 5 (empty)
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
    s.tap("A", settle=300)                                  # TAKE the item off slot 5
    s.shot("07_item_taken", "S150-3 review F1: TAKE'd MASTER BALL off the fresh mon in "
           "ITEM cursor mode -- footer reads 'A give  B put back'")
    s.press_n("LEFT", 5, settle=150)                        # slot 5 -> slot 0 (native CHIKORITA)
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
    # MOVE the same real mon (now back at slot 5) and try to drop/swap it onto the
    # native CHIKORITA cell in the SAME box -- the destination-side bc_is_native() guard
    # (immediately before the cross-box refusal) must deny before any memcpy, keeping
    # the native cell displayed and the carry still in hand.
    s.tap("A", settle=150)                                  # menu on slot 5 again
    s.press_n("DOWN", 3, settle=150)                        # VIEW/EDIT -> ITEM -> LEGALITY -> MOVE
    s.tap("A", settle=300)                                  # select MOVE -> pick it up
    s.shot("11_carrying_real_mon", "S150-3 review F2: MOVE picked up the real (non-native) "
           "mon from slot 5")
    s.press_n("LEFT", 5, settle=150)                        # slot 5 -> slot 0 (native CHIKORITA), same box
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
           "stays at its own slot 5, nothing lost")
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


def main(argv=None) -> int:
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
    ap.add_argument("--m1-map", action="store_true",
                     help="M1 (BACKLOG #91): only run_m1_map() against --image -- "
                          "--image MUST be a Red-only fused image (Red.gb+Red.sav)")
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
    ap.add_argument("--b89-hof", choices=("red", "crystal"),
                     help="BACKLOG #89: only run_b89_hof() against --image for the "
                          "named game (Red's or Crystal's own Hall of Fame screen) "
                          "-- --image MUST be a ONE-ROM fused image matching this "
                          "choice, same BACKLOG #98 harness-gap reasoning as "
                          "--u4-bag/--u5-pack/--b90-fly")
    ap.add_argument("--b89-hof-extra", choices=("red-nick", "crystal-nick", "crystal-shiny"),
                     help="BACKLOG #89 D6/NICK: only run_b89_hof_detail_only() -- "
                          "--image MUST be a ONE-ROM fused image whose .sav was "
                          "byte-poked first (a nicknamed mon for *-nick, a shiny DV "
                          "quad for crystal-shiny) -- see run_b89_hof_detail_only()'s "
                          "own docstring")
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
                          "named game's own summary portrait, through BOTH the "
                          "boot-picker mount and the nested START > NV_GB import "
                          "mount -- --image MUST be a COMBINED image (Emerald.sav + "
                          "Red/Gold/Crystal, `make delta-gb`'s own recipe) so both "
                          "mounts are reachable from the one image.")
    ap.add_argument("--b64-import", choices=("gold", "red"),
                     help="BACKLOG #64 (review Fix 3): run_b64_import() against --image "
                          "for the named game -- --image MUST be a COMBINED image "
                          "(Emerald.sav + Red/Gold/Crystal, `make delta-gb`'s own recipe, "
                          "same as run_nav_gb()). Drives the SAME START > nav menu > GB "
                          "import (NV_GB) entry run_nav_gb() already uses, now a REAL "
                          "streamed read-only session after Fix 3 gave pdna_gen12_show_"
                          "fused() its own Option B install -- captures Trainer/Bag-or-"
                          "Pack/Flags/Dex/Map from BOTH this mount AND the boot picker's "
                          "own direct GB row (the pre-existing resident-image mount) over "
                          "the SAME save, and `cmp`s each pair. The still-HW-only half is "
                          "pdna_gen12_show() itself (the real FIL/SD file-browser entry) "
                          "-- see run_b64_import()'s own docstring.")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "gb")
    ap.add_argument("--cold-start-compare", nargs=2, type=Path, metavar=("LOC_IMAGE", "NOLOC_IMAGE"),
                     help="BACKLOG #68b: measure+report the box-grid cold-start frame cost "
                          "on LOC_IMAGE (fused with the default rom_gb*_open_loc() records) "
                          "vs NOLOC_IMAGE (fused with `fuse_gb.py --no-loc`), diff their final "
                          "frames, and write both as shots into --out. Skips the normal "
                          "--image shot run entirely.")
    a = ap.parse_args(argv)
    a.out.mkdir(parents=True, exist_ok=True)

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
                "nicknamed via a byte-poked .sav (gb_name_encode) -- proves "
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
                "AND nicknamed at Lv 100 via a byte-poked .sav -- the exact worst "
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

    try:
        sess = run_standalone(core_mod, image_mod, a.image, a.out)
        ok += sess.taken
        skipped += sess.skipped
    except RuntimeError as e:
        print(f"  [STOPPED] boot-picker/standalone: {e}")
    for which in ("gold", "red"):
        try:
            sess = run_nav_gb(core_mod, image_mod, a.image, a.out, which)
            ok += sess.taken
            skipped += sess.skipped
        except RuntimeError as e:
            print(f"  [STOPPED] {which}: {e}")

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
    boot picker entirely -- exactly run_d7_gold()'s own nav, ONE tap (A: S1 info ->
    box grid), not the DOWN+A+A a combined multi-ROM image needs.

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

    s.press_n("DOWN", 3, settle=gb_shots.SETTLE)            # -> Release all
    s.shot("14_release_all_selected", "BACKLOG #93: cursor on Release all")
    s.tap("A", settle=gb_shots.BIG_SETTLE)                  # -> app_confirm with the count
    s.shot("15_release_all_confirm", "BACKLOG #93: 'Release all N Pokemon?' / "
                                      "'Deleted permanently!' -- release_box_all's "
                                      "own string (pdna_box.c), reused verbatim, N "
                                      "matching this box's real occupied count")
    # 20 individual gbs_delete() calls (top-down, one per occupied slot) take real
    # frames to settle -- measured by hand: BIG_SETTLE (40) is nowhere near enough
    # (the confirm dialog is still on screen), 700 reliably reaches gb_persist's wall.
    s.tap("A", settle=700)                                  # confirm -> delete top-down -> gb_persist
    s.shot("16_release_all_delta_wall", "BACKLOG #93: every slot deletes top-down "
                                         "(RAM-only), THEN gb_persist('release-all') "
                                         "hits the same PDNA_DELTA wall -- the fix in "
                                         "this lane's own follow-up commit makes sure "
                                         "this is the ONLY dialog shown here (an "
                                         "earlier version double-messaged with a "
                                         "second, FALSE 'Nothing changed' panel on "
                                         "top of it -- caught by this exact shot)")
    s.tap("A", settle=250)                                  # dismiss -> box grid, no second dialog
    s.shot("17_grid_after_release_all", "BACKLOG #93: straight to the box grid after "
                                         "ONE dismiss -- 0/20 (or 0/whatever this box "
                                         "held), every deletion landed in g_ed->img "
                                         "despite the persist refusal, exactly like "
                                         "shot 09's single-slot case")

    # Bank-cell absence (#120 S2's own bank_edge UP hop, reused verbatim from
    # run_s2_bank -- 3 UPs from a fresh grid entry: cell -> title -> tabs -> the hop).
    s2 = gb_shots.Session(core_mod, image_mod, rom, out_dir, f"b93_{which}_bank_")
    s2.run(700)
    s2.tap("A", settle=60)
    s2.run(GB_ART_COLD_SETTLE)
    s2.press_n("UP", 3, settle=100)
    s2.shot("18_bank_hop", "BACKLOG #93: the bank_edge UP hop opens the Bank -- "
                            "'BANK 1  0/30', every cell empty (#120 S2's F1 fix: "
                            "no write surface survives into a GB session's Bank "
                            "visit, so nothing can ever land here in mGBA)")
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
    s2.shot("19_bank_empty_cell_structural_proof",
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
                        own red path: boot picker DOWN -> A (S1 info) -> A (box grid) ->
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
                        path: A (S1 info) -> box grid -> START -> nav menu -> DOWN x7 ->
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


if __name__ == "__main__":
    sys.exit(main())
