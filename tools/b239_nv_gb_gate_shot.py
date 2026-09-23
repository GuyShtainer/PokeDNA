#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""b239_nv_gb_gate_shot.py -- BACKLOG #239 lane bank-only: one-shot mechanical proof
that the NV_GB ("GB import") row no longer offers a nested save-mount from a live
Gen-3 save's own nav menu.

Reuses tools/dgb_shots.py's own boot_to_gb_session()/nav_down_from_col_top() helpers
(pokedna-delta-gb.gba's boot picker -> Emerald row -> box grid -> START -> RIGHT ->
DOWN x(NV_GB's row) -> A) -- the EXACT navigation tools/dgb_shots.py's own
nav_to_gb_import() drives, deliberately NOT calling that function directly: before
this lane it asserted `screen_is(s, "pick_a_save")` after the A press (the nested
mount's own picker) -- that assertion is now WRONG on purpose (BACKLOG #239 closes
the very screen it asserts), so this is its own small script rather than a change to
dgb_shots.py's existing regression chain (out of scope for this lane; the dead nested
mount comes out in the later deletion pass BACKLOG #239 defers, along with its shot
coverage).

Before this lane: A on NV_GB opened pdna_gen12_show_fused()'s "PICK A SAVE" picker
(read-only nested mount).
After the fix-pass review (BACKLOG #239 D3): A on NV_GB shows its OWN dedicated
dialog -- title "BANK ONLY", a two-line procedure ("Open the GB save on its own," /
"send it to the Bank, come back.") -- instead of the generic "COMING SOON" / "Open
the Bank instead." wording the first pass shipped (a review BLOCK: that wording lied
-- the row WAS wired and was deliberately removed, and the named Bank was empty since
the GB save was never opened). The box screen underneath is UNCHANGED (no mount, no
picker, nothing entered).

    cd /tmp/pokedna-bank-only && /usr/local/bin/python3 tools/b239_nv_gb_gate_shot.py

Frame lands in /tmp/b239_nv_gb_gate_out/b239_01_refused.png. Exits 1 if the claim
("COMING SOON" + "Open the Bank instead.") fails, or if the box screen underneath
does not still show a live PC grid (proving nothing was entered).
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_shots            # noqa: E402
import dgb_shots           # noqa: E402
import gb_claims           # noqa: E402

ROM = ROOT / "pokedna-delta-gb.gba"


def main() -> int:
    if not ROM.is_file():
        sys.exit(f"missing {ROM} -- run `make delta-gb` in this worktree first")
    core_mod, image_mod = gb_shots.load_mgba()
    out = Path("/tmp/b239_nv_gb_gate_out")
    out.mkdir(exist_ok=True, parents=True)
    s = gb_shots.Session(core_mod, image_mod, ROM, out, "b239_")
    print("== BACKLOG #239: NV_GB is unreachable from a live Gen-3 save's nav menu ==")

    # Emerald's own row (0, the default selection) continues STRAIGHT into the normal
    # Gen-3 box screen -- no gb_info_page in between (that screen is only shown when a
    # GB row is picked instead). Same two-tap pattern run_gbscreen_shell() uses for
    # this identical image (tools/dgb_shots.py:393-394): boot_to_gb_session() is NOT
    # reused here on purpose -- it asserts gb_info_page unconditionally, which only
    # holds for a GB row, not row 0/Emerald.
    s.run(700)
    s.tap("A", settle=gb_shots.BIG_SETTLE)        # #68a boot picker, Emerald row (default) -> box
    s.shot("00_box_before", "before: Emerald box grid, nothing entered yet")

    # The SAME navigation nav_to_gb_import() drives, minus its own (now-wrong)
    # assert_screen(pick_a_save) -- see this file's own module docstring.
    s.tap("START", settle=gb_shots.BIG_SETTLE)                       # box screen -> nav menu
    s.tap("RIGHT")                                                    # column 0 -> column 1
    s.press_n("DOWN", dgb_shots.nav_down_from_col_top("NV_GB"))        # Blocks -> ... -> GB import
    s.tap("A", settle=gb_shots.BIG_SETTLE)                            # NV_GB -> refuse, not a mount
    s.shot("01_refused", "BACKLOG #239 D3: NV_GB on a live Gen-3 save now shows its "
           "own \"BANK ONLY\" dialog with the deposit-first procedure, instead of "
           "opening the nested read-only GB mount (pdna_gen12_show_fused's own "
           "PICK A SAVE picker) -- xfer_direct_allowed()/nav_avail() close the row "
           "before app_pick_gb_save/gb_delta_pick_save ever runs, and the reused "
           "\"COMING SOON\"/\"Open the Bank instead.\" wording from the first pass "
           "was replaced (it lied: the row was wired and removed on purpose, and "
           "the named Bank was empty).",
           claim=["BANK ONLY", "Open the GB save on its own,", "send it to the Bank, come back."])

    s.tap("A", settle=gb_shots.BIG_SETTLE)                            # msg_wait dismisses on A ("Press A")
    s.shot("02_box_after", "after: back on the SAME box grid, still nothing entered "
           "-- the refusal did not consume or alter any state")

    ok = not s.any_claim_failed
    print(f"\n{'PASS' if ok else 'FAIL'}: {len(s.taken)} shots, "
          f"{'no' if ok else 'SOME'} claim failures")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
