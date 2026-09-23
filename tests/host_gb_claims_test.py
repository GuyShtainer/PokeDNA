#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""host_gb_claims_test.py -- self-test for tools/gb_claims.py (BACKLOG #184).

Pure Python + numpy/PIL, no emulator/mgba import (same house rule the other harness
self-tests follow: "harness scripts never `import mgba` at module level"). Covers:

  (a) render("PASTE HERE", proportional=True/False) into a synthetic frame at a
      RANDOM position with a RANDOM ink colour on a noisy (multi-colour, small-palette)
      background, and find() locates it -- both font modes.
  (b) a near-miss ("PASTE HERO" instead of "PASTE HERE") painted the SAME way is NOT
      found -- proves this is an exact bitmap match, not a fuzzy/substring one.
  (c) find(proportional=None) (the caller-agnostic mode Session.shot() actually uses)
      finds text drawn in EITHER font without being told which.
  (d) claim_absent semantics via check(): a claim that IS present fails claim_absent;
      a claim that is genuinely absent passes it.
  (e) a real PNG fixture, if one is tracked under docs/shots -- else a second
      synthetic frame stands in (docs/ is gitignored in this repo, so a tracked
      fixture is not guaranteed to exist in every checkout).

Run directly:

    python3 tests/host_gb_claims_test.py

Registered in tests/run_host_tests.py's PY_TESTS list (BACKLOG #184).
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import numpy as np  # noqa: E402
import gb_claims  # noqa: E402

# Same corpus convention tests/run_host_tests.py itself uses (BACKLOG #214's
# claim_gb= self-test): overridable via $ROMS, defaulting to Guy's own
# machine-local dump directory -- READ-ONLY, never opened for write.
ROMS = Path(os.environ.get("ROMS", "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms"))

# BACKLOG #254: docs/ is gitignored in every PokeDNA worktree (see
# docs/worktree_setup.sh's own copy list, which does not include docs/shots), so a
# lane worktree never has docs/shots/gb -- only the main checkout does. Same
# "hardcoded machine-local default, overridable, skip cleanly if absent" shape as
# ROMS right above, not a new convention.
SHOTS_GB = Path(os.environ.get(
    "SHOTS_GB", "/Users/guyshtainer/VSCodeProjects/gba-toolkit/projects/PokeDNA/docs/shots/gb"))


def make_noisy_frame(rng, h=160, w=240, palette_size=24):
    """A small-palette noisy background -- realistic for a GBA capture (<=256
    colours, gb_claims.find()'s own documented assumption), not per-pixel 24-bit
    noise (which would blow past that cap and is not what mGBA ever produces)."""
    palette = rng.integers(0, 256, size=(palette_size, 3), dtype=np.uint8)
    idx = rng.integers(0, palette_size, size=(h, w))
    return palette[idx]


def paint_text(frame, text, x0, y0, colour, proportional, bg=(10, 10, 10)):
    mask = gb_claims.render(text, proportional=proportional)
    h, w = mask.shape
    frame[y0:y0 + h, x0:x0 + w] = bg
    frame[y0:y0 + h, x0:x0 + w][mask] = np.array(colour, dtype=np.uint8)
    return mask.shape


def check_random_position(rng, text, proportional, label, failures):
    frame = make_noisy_frame(rng)
    colour = tuple(int(c) for c in rng.integers(40, 256, size=3))
    # Keep well clear of the frame edge so the whole glyph run fits.
    h, w = gb_claims.render(text, proportional=proportional).shape
    x0 = int(rng.integers(0, 240 - w))
    y0 = int(rng.integers(0, 160 - h))
    paint_text(frame, text, x0, y0, colour, proportional)

    hits = gb_claims.find(frame, text, proportional=None)  # caller-agnostic mode
    if not any(x == x0 and y == y0 for x, y, c in hits):
        failures.append(f"{label}: render({text!r}, proportional={proportional}) at "
                         f"({x0},{y0}) colour {colour} was NOT found by "
                         f"find(proportional=None); hits={hits}")
        return
    match = [c for x, y, c in hits if x == x0 and y == y0][0]
    if match != colour:
        failures.append(f"{label}: found at the right position but wrong colour "
                         f"{match} != {colour}")
        return

    # near-miss: same position/colour, one character changed -- must not match.
    near = text[:-1] + ("O" if text[-1] != "O" else "X")
    near_hits = gb_claims.find(frame, near, proportional=None)
    if any(x == x0 and y == y0 for x, y, c in near_hits):
        failures.append(f"{label}: near-miss {near!r} incorrectly matched at "
                         f"({x0},{y0}) -- find() is not exact")
        return

    print(f"  ok: {label} render({text!r}, proportional={proportional}) found at "
          f"({x0},{y0}) colour {colour}; near-miss {near!r} correctly rejected")


def check_claim_absent(rng, failures):
    frame = make_noisy_frame(rng)
    present_text = "KEEP"
    absent_text = "DISCARD ALL"
    colour = (255, 255, 255)
    h, w = gb_claims.render(present_text, proportional=True).shape
    x0, y0 = 50, 30
    paint_text(frame, present_text, x0, y0, colour, proportional=True)

    fails = gb_claims.check(frame, claim=present_text)
    if fails:
        failures.append(f"claim_absent setup: claim={present_text!r} unexpectedly "
                         f"failed: {fails}")
        return
    fails = gb_claims.check(frame, claim_absent=present_text)
    if not fails:
        failures.append(f"claim_absent: {present_text!r} IS on the frame but "
                         f"claim_absent did not fail")
        return
    fails = gb_claims.check(frame, claim_absent=absent_text)
    if fails:
        failures.append(f"claim_absent: {absent_text!r} is genuinely absent but "
                         f"claim_absent reported a failure: {fails}")
        return
    print("  ok: check() claim_absent semantics (present text fails claim_absent, "
          "genuinely-absent text passes it)")


def check_real_fixture(failures):
    """A real PNG from docs/shots, if this checkout happens to have one tracked
    (docs/ is gitignored -- see docs/kb/gitignore-internal-dev-docs.md's convention
    -- so most checkouts will not). Falls back to a second synthetic frame so the
    test still exercises find() against a full PIL round-trip (path -> Image ->
    ndarray), not just an in-memory ndarray, in every checkout."""
    shots_dir = ROOT / "docs" / "shots" / "gb"
    manifest_path = shots_dir / "manifest.json"
    fixture = None
    if manifest_path.is_file():
        import json
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
            for entry in manifest.get("shots", []):
                p = shots_dir / entry.get("file", "")
                if p.is_file():
                    fixture = p
                    break
        except Exception:
            fixture = None

    if fixture is not None:
        # We don't know this fixture's own text in advance (it's whatever the
        # checkout happens to have) -- the only thing we can assert without
        # coupling to a specific shot is that find() runs to completion and
        # returns a list (no exception) for an arbitrary short claim.
        hits = gb_claims.find(fixture, "A", proportional=None)
        if not isinstance(hits, list):
            failures.append(f"real fixture {fixture}: find() did not return a list")
            return
        print(f"  ok: real fixture {fixture.relative_to(ROOT)} -- find() ran clean "
              f"({len(hits)} hit(s) for a single-glyph probe)")
    else:
        rng = np.random.default_rng(999)
        frame = make_noisy_frame(rng)
        paint_text(frame, "REAL FIXTURE STAND-IN", 10, 100, (0, 255, 0), True)
        from PIL import Image
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "stand_in.png"
            Image.fromarray(frame, "RGB").save(path)
            hits = gb_claims.find(path, "REAL FIXTURE STAND-IN", proportional=None)
            if not any(x == 10 and y == 100 for x, y, c in hits):
                failures.append("stand-in fixture (no tracked docs/shots PNG in this "
                                 f"checkout): find() via a real file path failed; hits={hits}")
                return
        print("  ok: no tracked docs/shots/gb PNG in this checkout (docs/ is "
              "gitignored) -- stand-in file-path round trip passed instead")


def check_many_colours(rng, failures):
    """BACKLOG #214: a >256-distinct-colour frame (a sprite/gameplay screen)
    must never raise -- find() bounds its per-colour search to the most
    frequent colours (gb_claims.MAX_COLOURS_SCANNED) instead. A 300-colour
    frame with "PASTE HERE" drawn in one (high-frequency, since it is many
    ink pixels) colour is still found; the same frame WITHOUT the text
    returns [] with no exception."""
    h, w = 160, 240
    # 300 distinct colours: a smooth-ish gradient so no colour repeats often
    # on its own, keeping every art colour's pixel count low relative to the
    # text ink (which will cover dozens of pixels in one colour).
    n_colours = 300
    palette = rng.integers(0, 256, size=(n_colours, 3), dtype=np.uint8)
    idx = rng.integers(0, n_colours, size=(h, w))
    frame_with_text = palette[idx].copy()
    frame_without_text = palette[idx].copy()

    text_colour = (255, 255, 255)
    x0, y0 = 40, 60
    paint_text(frame_with_text, "PASTE HERE", x0, y0, text_colour, proportional=True,
               bg=(0, 0, 0))

    n_actual = len(np.unique(frame_with_text.reshape(-1, 3), axis=0))
    if n_actual <= 256:
        failures.append(f"check_many_colours: fixture only has {n_actual} distinct "
                         "colours (need >256) -- test setup is not exercising the "
                         "bounded-scan path")
        return

    try:
        hits = gb_claims.find(frame_with_text, "PASTE HERE", proportional=True)
    except Exception as exc:  # noqa: BLE001 -- proving find() never raises here
        failures.append(f"check_many_colours: find() RAISED on a {n_actual}-colour "
                         f"frame instead of returning a bounded result: {exc!r}")
        return
    if not any(x == x0 and y == y0 for x, y, c in hits):
        failures.append(f"check_many_colours: 'PASTE HERE' on a {n_actual}-colour "
                         f"frame was not found at ({x0},{y0}); hits={hits}")
        return

    try:
        hits_absent = gb_claims.find(frame_without_text, "PASTE HERE", proportional=True)
    except Exception as exc:  # noqa: BLE001
        failures.append(f"check_many_colours: find() RAISED on a >256-colour frame "
                         f"without the claim text instead of returning []: {exc!r}")
        return
    if hits_absent:
        failures.append(f"check_many_colours: 'PASTE HERE' incorrectly found on the "
                         f"frame that never drew it; hits={hits_absent}")
        return

    print(f"  ok: >256-colour frame ({n_actual} colours) -- find() never raises; "
          "text-present frame matched, text-absent frame returned []")


def check_claim_gb(failures):
    """BACKLOG #214 item 2: claim_gb='s matcher against a REAL corpus ROM's
    own font (never a literal in this repo -- see gb_claims.py's claim_gb=
    design comment). Skips with a clear print (not a silent pass) if the
    corpus isn't present on this machine.

    (a) render_gb(rom, text) produces an (8, 8*n) bool mask whose glyph count
        matches the text's own length (no lossy/blank glyphs for plain
        ASCII).
    (b) find_gb() locates that mask painted at a KNOWN gbscr 1:1-grid cell
        position (GBSCR_ORIGIN_X/Y + col/row*8) on a synthetic 240x160
        frame, and does NOT locate it at a position one pixel off that grid
        (proving the grid-alignment requirement is real, not accidental).
    (c) a text this ROM's font cannot possibly show (letters the corpus ROM
        was never asked to draw are still just its own charmap -- so the
        negative control here is a position with NO painted glyphs at all)
        returns [] via find_gb(), never an exception.
    (d) check_gb() wraps this with claim_gb='s check()-shaped failure list."""
    rom = ROMS / "gb" / "Red.gb"
    if not rom.is_file():
        print(f"  skip: check_claim_gb -- corpus ROM not present at {rom} "
              "(set $ROMS to point at a gba-toolkit roms/ checkout)")
        return

    text = "7 teams"
    mask = gb_claims.render_gb(rom, text)
    if mask is None:
        failures.append(f"check_claim_gb: render_gb({rom}, {text!r}) returned None "
                         "(font locate or gb_char_encode failed on a corpus ROM "
                         "that should locate cleanly)")
        return
    expect_glyphs = len(text)   # every char here is plain ASCII -> 1 glyph each
    if mask.shape != (gb_claims.GB_CELL, gb_claims.GB_CELL * expect_glyphs):
        failures.append(f"check_claim_gb: render_gb mask shape {mask.shape}, "
                         f"expected (8, {gb_claims.GB_CELL * expect_glyphs})")
        return

    col, row = 3, 5
    x0 = gb_claims.GBSCR_ORIGIN_X + col * gb_claims.GB_CELL
    y0 = gb_claims.GBSCR_ORIGIN_Y + row * gb_claims.GB_CELL
    frame = np.full((160, 240, 3), 255, dtype=np.uint8)   # gbscr's own blank colour
    crop = frame[y0:y0 + mask.shape[0], x0:x0 + mask.shape[1]]
    crop[mask] = (16, 16, 16)   # DMG's darkest shade -- rom_gbui.c's own DMG_SHADE[3]

    hits = gb_claims.find_gb(frame, rom, text)
    if (x0, y0) not in hits:
        failures.append(f"check_claim_gb: {text!r} painted at ({x0},{y0}) on the "
                         f"real gbscr grid was NOT found by find_gb(); hits={hits}")
        return

    # Off-grid: shift the SAME painted glyphs one pixel right of their true
    # cell boundary -- find_gb() only tries cell-aligned x/y, so this frame
    # must report NO hit at the (now wrong) grid position, proving the
    # search is grid-locked rather than accidentally sliding.
    frame_off = np.full((160, 240, 3), 255, dtype=np.uint8)
    frame_off[y0:y0 + mask.shape[0], x0 + 1:x0 + 1 + mask.shape[1]][mask] = (16, 16, 16)
    hits_off = gb_claims.find_gb(frame_off, rom, text)
    if (x0, y0) in hits_off:
        failures.append("check_claim_gb: a 1px-off-grid paint incorrectly matched "
                         f"at the true grid cell ({x0},{y0}); hits={hits_off}")
        return

    # A blank frame (nothing painted) must report no hit and no exception.
    blank = np.full((160, 240, 3), 255, dtype=np.uint8)
    hits_blank = gb_claims.find_gb(blank, rom, text)
    if hits_blank:
        failures.append(f"check_claim_gb: blank frame incorrectly matched; "
                         f"hits={hits_blank}")
        return

    fails = gb_claims.check_gb(frame, rom, claim_gb=text)
    if fails:
        failures.append(f"check_claim_gb: check_gb() reported a failure on the "
                         f"correctly-painted frame: {fails}")
        return
    fails = gb_claims.check_gb(blank, rom, claim_gb=text)
    if not fails:
        failures.append("check_claim_gb: check_gb() did not fail on a blank frame")
        return

    print(f"  ok: claim_gb= against the real corpus ROM {rom.name} -- {text!r} "
          f"found at its true grid cell, rejected 1px off-grid, absent on a "
          f"blank frame, check_gb() failure semantics hold")


def check_cursor_occlusion(failures):
    """BACKLOG #254: the GB shell's row/field cursor (m3_frame, GBCARD_CSEL --
    source/pdna_gbhof.c:54 / source/pdna_gbtrainer.c:48) is drawn AFTER the text and
    overwrites the SELECTED row's own top/bottom border scanline with one hardcoded
    colour -- the old strict find_gb() missed a species name that WAS on the frame
    whenever its row was selected (docs/shots/gb/b89_crystal_02_detail.png,
    claim_gb=TYPHLOSION was the original repro). This is the real-frame regression
    corpus for the fix: two selected-row positives (the exact repro plus a second,
    different-game/no-icon frame), four unselected-row controls on the SAME two
    frames (the common, unoccluded case must still match), two negative controls
    (genuinely absent text, and a same-length WRONG string on the very same
    selected row/position as the real match -- proves the cursor-colour exclusion
    is not "any text of the right length matches"), and a mutation of
    GB_CURSOR_OCCLUSION_CAP itself (BACKLOG #254's own honesty requirement: prove
    the refusal path is real, not decoration -- see STANDING-RULES.md's
    pre-report self-audit item 1). Skips cleanly (not a silent pass -- prints why)
    if this machine doesn't have the gitignored docs/shots/gb corpus or the ROM
    corpus, same shape check_claim_gb() already uses."""
    if not SHOTS_GB.is_dir():
        print(f"  skip: check_cursor_occlusion -- {SHOTS_GB} not present on this "
              "machine (docs/ is gitignored; set $SHOTS_GB to override)")
        return
    crystal_rom = ROMS / "gb" / "Crystal.gbc"
    red_rom = ROMS / "gb" / "Red.gb"
    crystal_png = SHOTS_GB / "b89_crystal_02_detail.png"
    red_png = SHOTS_GB / "b89_red_02_detail.png"
    missing = [p for p in (crystal_rom, red_rom, crystal_png, red_png) if not p.is_file()]
    if missing:
        print(f"  skip: check_cursor_occlusion -- missing on this machine: "
              f"{[str(p) for p in missing]}")
        return

    # (1) positives: the SELECTED row's own species name IS found, on two
    # independent frames (Gen 2 with a 16px icon column, Gen 1 with none --
    # BACKLOG #254's own finding was that the icon never actually reaches the
    # text columns on either repro; both still exercise the top/bottom border
    # scanline corruption every selected row gets).
    if not gb_claims.find_gb(crystal_png, crystal_rom, "TYPHLOSION"):
        failures.append("check_cursor_occlusion: 'TYPHLOSION' (row 0, SELECTED, "
                         "b89_crystal_02_detail.png) not found -- BACKLOG #254 regressed")
        return
    if not gb_claims.find_gb(red_png, red_rom, "MEW"):
        failures.append("check_cursor_occlusion: 'MEW' (row 0, SELECTED, "
                         "b89_red_02_detail.png) not found -- BACKLOG #254 regressed")
        return

    # (2) unselected-row controls on the SAME two frames: the ordinary,
    # unoccluded case must still match exactly as before this fix.
    for png, rom, text in [(crystal_png, crystal_rom, "NOCTOWL"),
                            (crystal_png, crystal_rom, "MANTINE"),
                            (red_png, red_rom, "MEWTWO"),
                            (red_png, red_rom, "CHARIZARD")]:
        if not gb_claims.find_gb(png, rom, text):
            failures.append(f"check_cursor_occlusion: unselected-row control "
                             f"{text!r} on {png.name} not found")
            return

    # (3) negative control: genuinely absent text on the selected-row frame.
    fails = gb_claims.check_gb(crystal_png, crystal_rom, claim_gb="BULBASAUR")
    if not fails or "not on frame" not in fails[0]:
        failures.append(f"check_cursor_occlusion: 'BULBASAUR' (genuinely absent) "
                         f"should fail as 'not on frame', got {fails}")
        return

    # (4) negative control: a same-length WRONG string at the SAME
    # selected-row position as the real match -- the cursor-colour exclusion
    # must not degrade into "any text of the right length matches here".
    fails = gb_claims.check_gb(crystal_png, crystal_rom, claim_gb="TYPHLOSIAN")
    if not fails or "not on frame" not in fails[0]:
        failures.append(f"check_cursor_occlusion: near-miss 'TYPHLOSIAN' (same "
                         f"length as the real, selected TYPHLOSION) should still "
                         f"fail as 'not on frame', got {fails}")
        return

    # (5) mutation: GB_CURSOR_OCCLUSION_CAP must be load-bearing. Forcing it to
    # 0.0 means ANY cursor-colour pixel at all triggers a refusal, so the real,
    # correct 'TYPHLOSION' match (which genuinely has occluded ink pixels on
    # this frame) must now come back as the DISTINCT "cannot be verified"
    # wording, not "not on frame" and not a silent pass -- proving this is a
    # real code path this test can turn red, per STANDING-RULES.md's
    # pre-report self-audit item 1, not an assertion that has never failed.
    real_cap = gb_claims.GB_CURSOR_OCCLUSION_CAP
    try:
        gb_claims.GB_CURSOR_OCCLUSION_CAP = 0.0
        fails = gb_claims.check_gb(crystal_png, crystal_rom, claim_gb="TYPHLOSION")
    finally:
        gb_claims.GB_CURSOR_OCCLUSION_CAP = real_cap
    if not fails or "cannot be verified" not in fails[0]:
        failures.append("check_cursor_occlusion: forcing GB_CURSOR_OCCLUSION_CAP="
                         f"0.0 should have produced the distinct 'cannot be "
                         f"verified' refusal for 'TYPHLOSION', got {fails}")
        return
    fails = gb_claims.check_gb(crystal_png, crystal_rom, claim_gb="TYPHLOSION")
    if fails:
        failures.append(f"check_cursor_occlusion: 'TYPHLOSION' should pass again "
                         f"once GB_CURSOR_OCCLUSION_CAP is restored, got {fails}")
        return

    print("  ok: cursor-highlight-aware claim_gb= matcher (BACKLOG #254) -- 2 "
          "selected-row positives, 4 unselected-row controls, 2 negative controls, "
          "1 occlusion-cap mutation, all real frames from docs/shots/gb, all hold")


def main() -> int:
    failures: list[str] = []
    rng = np.random.default_rng(20260922)

    check_random_position(rng, "PASTE HERE", True, "proportional", failures)
    check_random_position(rng, "PASTE HERE", False, "fixed (tonc sys8)", failures)
    check_claim_absent(rng, failures)
    check_real_fixture(failures)
    check_many_colours(rng, failures)
    check_claim_gb(failures)
    check_cursor_occlusion(failures)

    if failures:
        print(f"\nhost_gb_claims_test: {len(failures)} FAILURE(S):", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print("\nhost_gb_claims_test: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
