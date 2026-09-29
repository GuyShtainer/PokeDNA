#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""gb_shots.py — headless mGBA screenshots of the Gen-1/2 arc (S1..S5-B).

WHY THIS EXISTS
----------------
The Game Boy fork in source/pdna_main.c's view_save() lives entirely inside
`#ifndef PDNA_DELTA` — the emulator build has never been able to reach it, because
that build has no SD card to read a .sav off of. Nobody has ever had a screenshot
of S1..S5-B. docs/HANDOFF.md 2026-09-05 added a narrow, delta-only path: when the
image fused into pokedna-delta.gba (tools/fuse_sav.py --gb) is a Game Boy save
instead of a Gen-3 one, view_save() short-circuits straight into
pdna_gen12_show_image() and loops there. That is the only door this script needs.

WHAT THIS DRIVES
----------------
Two fused images (built by the caller, this script does not fuse anything):
  pokedna-delta.gba + Gold.sav (+ --clip, an 80-byte real Gen-3 box record) — the
    Gen-2 run: info page, box grid, mon menu, the native summary (BACKLOG #41,
    source/pdna_gbsummary.c) in VIEW across all FOUR cards then in EDIT (+ a DV
    edit), the confirm screen, MOVE TO, RELEASE, and — because a clip was seeded —
    PASTE (GB) on an empty cell through to the loss screen.
  pokedna-delta.gba + Red.sav (no clip) — the Gen-1 run: box grid, the native
    summary VIEW (card 0 INFO + card 3 ORIGIN, which shows the record's own G1
    type bytes) + EDIT (no Item/Friendship rows — gbe_fields() drops them outside
    GB_GEN2), and the "Gen 1: withdraw it in-game instead" refusal MOVE TO -> Party
    produces (source/pdna_layout.h PDNA_GBEDIT_MOVE_NEEDSBASE_L2, source/pdna_gen12.c).

BACKLOG #51/#50 (2026-09-07, this session): run_gold_gender() shows the new Gender
row (source/gb_editor.c's GBE_GENDER) flipping Bulbasaur M -> F live, header and
all. run_gold_create()/run_red_create() drive the CREATE action on an empty cell
(source/pdna_gen12.c's gb_create_hook) through the species picker. (BACKLOG #278:
Gen 1 no longer hits a "Needs your Gen 1/2 ROM" wall with no ROM -- the generated
base table builds the level-1 mon and run_red_create() shoots its summary; Gen 2's
run_gold_create() is unchanged.)

BACKLOG #50 UX-parity re-shoot (2026-09-07/08, Guy: "Pokemon creation is not from
the Pokedex view -- fix that", "I want the Gen-1/2 versions to look the same from
the start"): CREATE now opens the SAME pdna_pick.c pick_species() the Gen-3 create
flow uses (icon grid, filters, search), restricted to the session's own generation
via pick_species_set_max_dex() -- replacing the earlier bespoke "No.NNN NAME" text
list this file used to shoot (gb_create_pick_species, deleted). There is also no
level PROMPT any more (gb_create_pick_level, also deleted): rom_gblearn_min_level()
computes the species' own lowest legal level, mirroring gen3_build_level's "5 for a
Bulbasaur, 36 for a Charizard". Two new shots per generation demonstrate the
restriction itself: cycling the picker's OWN filter with R/L never lands on a
generation the session's ceiling has already excluded (filter_usable(),
pdna_pick.c). Filter id 1 ("Gen 1") is never itself excluded, so the first R
always stops there normally in EITHER session; it is the SECOND R that then jumps
straight over whatever the ceiling has excluded in one keypress -- "Gen 3+" only,
for a Gen-2 session's picker (3 presses total to reach Legendary), or BOTH "Gen 2"
and "Gen 3+" together, for a Gen-1 session's (2 presses total). The species
picker, the filter-skip demo and, for Gen 1, the ROM-free new-mon summary
(BACKLOG #278/#276 -- the generated base table builds it with no ROM) ARE
captured; Gen 2's build still needs a real ROM on a real SD card and is
hardware-only (docs/HW-TEST-2026-09-05-GB-ARC.md §O).

BACKLOG #50 UX-parity audit (2026-09-07, same session): run_gold()'s own
"03_mon_menu"/"07_move_to_picker"/"08_release_confirm" and run_red()'s own
"12d_move_to_party_selected" navigation and captions were updated for the
occupied-mon-menu row reorder + relabel (app_mon_menu_readonly, source/
pdna_main.c): LEGALITY now sits right after VIEW/EDIT and RELEASE is LAST,
matching Gen 3's own occupied-mon-menu order, and "MOVE TO" is relabelled "MOVE TO
BOX" (Gen 3's own label for the same destination-picker shape). Without this fix
those four shots would have silently captured the WRONG screen (a stale DOWN-count
landing on a different, since-moved row) -- caught by re-deriving the navigation
from the new row order before re-running this script, not by a mismatched
screenshot after the fact.

BACKLOG #41 slice E1 (2026-09-05/06, Guy: "I prefer the summary edit design to be
like we did for gen 3 ... [with] less editable stats"): source/pdna_gbsummary.c
restyled to the SAME Gen-3 CARD chrome pdna_summary.c uses — the shared left info
panel (portrait/name/level/type via the origin-art router), the card dots, and a
MOVING-OUTLINE selection (pdna_summary_sel_frame_*) instead of the earlier retail-
page design's inline per-row highlight box — over FOUR cards (INFO / SKILLS /
MOVES / ORIGIN, the last one new: the honest "this is a Gen 3 preview, not a real
transfer" story + the sidecar link status) instead of the original BACKLOG #41
three-card retail-page layout (INFO / STATS / MOVES, "STATS" renamed "SKILLS").
Shots 04*/05*/06*/07*/12b* below were re-shot for the new chrome and for the new
ORIGIN card; the navigation itself (L/R flips cards, U/D moves the field cursor)
was already Gen-3-parity from BACKLOG #41's first landing and is unchanged here.
BACKLOG #40's four cosmetic nits (box banner capacity, Gen-1 box-name spacing, the
paste-refusal title, the party-full wording) are also visible in several captions.

Every SD-backed action (a card write, i.e. anything past "confirm") refuses in the
emulator — mGBA has no flashcart. That refusal is captured too: it is honest
evidence the never-corrupt gate holds even with nowhere to write, not a bug.

Usage:
    /usr/local/bin/python3 tools/gb_shots.py --gold FUSED_GOLD.gba --red FUSED_RED.gba \
        --out docs/shots/gb

Frame-advance idiom (docs/HANDOFF.md's mGBA harness + the auto-repeat gotcha this
script's own trial run hit): every screen in this app enables d-pad AUTO-REPEAT
(key_repeat_mask), so a press held too long or fired right after a screen
transition can silently advance twice. Every tap here is a short hold + a long
release/settle, and every action that opens or closes a whole screen gets an
extra settle on top before the next tap is sent.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gb_claims  # noqa: E402 -- BACKLOG #184: claim=/claim_absent= mechanical check.
                   # No mgba import at module level in gb_claims.py (numpy/PIL only).
import fuse_gb  # noqa: E402 -- BACKLOG #214 item 2: claim_gb= extracts the embedded GB
                # ROM (TYPE_ROM_GEN1/2) straight out of the SAME fused .gba image this
                # Session booted, via fuse_gb's own directory parser (never a second
                # implementation of the fuse format). No mgba import at module level here
                # either (fuse_gb.py only touches struct/zlib/tempfile).
import vsd  # noqa: E402 -- BACKLOG #179 Phase A step A3: the harness-hosted virtual SD
            # server. No mgba import at module level here either (vsd.py only touches
            # struct/dataclasses/pathlib) -- see vsd.py's own header for the protocol.
import vsd_diff  # noqa: E402 -- BACKLOG #179 A3 review D1: Session.vsd_report()'s own
                  # diff, reusing vsd_diff.py's parser/differ rather than a second
                  # implementation. Pure stdlib (dataclasses/sys) -- no mgba import here either.

KEY = dict(A=0x1, B=0x2, SEL=0x4, START=0x8, RIGHT=0x10, LEFT=0x20,
           UP=0x40, DOWN=0x80, R=0x100, L=0x200)

HOLD = 3          # frames a key is physically "down"
SETTLE = 12       # frames of nothing, after a simple cursor move
BIG_SETTLE = 40   # frames of nothing, after a screen opens/closes/repaints fully
VSD_QUIESCE_IDLE = 4        # S4.5: consecutive no-request frames that mean "I/O settled"
VSD_QUIESCE_CAP = 3600      # S4.5: hard cap -- a chain that never quiesces is a loud failure

# BACKLOG #255: the #253 corpus was fourteen frames of the FULL-ART build shot against a
# chain whose docstring says "MUST be pokedna-delta-artless.gba" -- with art linked in, the
# first icon lookup succeeds and the dex opens in grid view instead of list view, so every
# frame documented a screen that was never on the glass, and every check anyone ran (file
# freshness, sheet regeneration, caption-vs-pixel on the frames people happened to open)
# passed. The marker a chain needs is source/build_variant.c's pdna_build_variant string,
# grep-able straight out of the built .gba (see that file for why a plain
# __attribute__((used)) data symbol did NOT survive this Makefile's --gc-sections link, and
# why the marker also has to be exercised by a live call site).
_VEHICLE_MARKER = {"ART": b"PDNA-VARIANT:ART", "ARTLESS": b"PDNA-VARIANT:ARTLESS"}


def assert_vehicle(image_path: "str | Path", want: str) -> str:
    """Refuse to shoot a chain against the wrong build.

    Reads image_path and greps it for the PDNA-VARIANT marker (source/build_variant.c).
    want is "ARTLESS" or "ART" (DELTA-ness is not checked here -- the caller already picked
    the delta vs non-delta image by path/filename; this only guards the art/artless axis,
    which is the one #253 got wrong).

    Returns the marker string found (e.g. "PDNA-VARIANT:ARTLESS+DELTA").
    Raises SystemExit(1) -- loud, chain-stopping, not a warning -- naming the image, what
    was found, and what was wanted, if the wrong variant (or no marker at all) is found.
    """
    if want not in _VEHICLE_MARKER:
        raise ValueError(f"assert_vehicle: want must be 'ART' or 'ARTLESS', got {want!r}")
    path = Path(image_path)
    data = path.read_bytes()
    # "PDNA-VARIANT:ART" is a byte-prefix of "PDNA-VARIANT:ARTLESS" -- check ARTLESS first
    # so an artless image is never mis-read as an ART-image false match on the prefix.
    if _VEHICLE_MARKER["ARTLESS"] in data:
        found = "ARTLESS"
    elif _VEHICLE_MARKER["ART"] in data:
        found = "ART"
    else:
        found = None
    if found != want:
        found_desc = (
            "PDNA-VARIANT:" + found if found else
            "(no PDNA-VARIANT marker at all -- image predates BACKLOG #255"
            " or was not built by this Makefile)"
        )
        make_hint = "make artless" if want == "ARTLESS" else "make (no PDNA_ARTLESS)"
        sys.exit(
            f"*** REFUSING: {path.name} is the WRONG build for this chain.\n"
            f"***   wanted:  PDNA-VARIANT:{want}\n"
            f"***   found:   {found_desc}\n"
            f"***   image:   {path}\n"
            f"*** BACKLOG #255: the #253 corpus was 14 frames of the wrong build and nothing\n"
            f"*** could tell. Build the right variant ({make_hint}) and re-run."
        )
    # Recover the exact marker text (including +DELTA, if present) for the caller to log.
    marker_with_delta = _VEHICLE_MARKER[found] + b"+DELTA"
    marker = marker_with_delta if marker_with_delta in data else _VEHICLE_MARKER[found]
    return marker.decode("ascii")

# BACKLOG #179 Phase A step A3: dgb_shots.py's --vsd CLI flag calls set_default_vsd()
# ONCE, before dispatching to whichever run_*() the user selected. Every Session()
# constructed afterwards in this process picks the image (and any failure-injection
# knobs) up automatically unless it passes its own vsd_img= explicitly -- avoids
# threading a new kwarg through every one of dgb_shots.py's 70+ run_*() call sites.
# When neither this nor a per-call vsd_img is set (every existing runner today, and
# any runner invoked without --vsd), Session.vsd stays None and behaves byte-identically
# to before this lane -- that equivalence is S4.3's own gate and A4's own merge
# condition, re-proven for every runner without --vsd.
_DEFAULT_VSD_IMG: "Path | None" = None
_DEFAULT_VSD_KNOBS: dict = {}


def set_default_vsd(img: "Path | None", **knobs) -> None:
    global _DEFAULT_VSD_IMG, _DEFAULT_VSD_KNOBS
    _DEFAULT_VSD_IMG = img
    _DEFAULT_VSD_KNOBS = knobs


# BACKLOG #179 A3 review D1 (BLOCKER): VsdImage.flush() had no caller anywhere in this
# tree -- every --vsd write lived only in the in-process bytearray and died with the
# script. Every Session that attaches a VSD registers itself here; dgb_shots.main()
# (and this module's own main() below) drains the list in a try/finally wrapped around
# its ENTIRE dispatch, so a flush happens exactly once per process regardless of which
# runner(s) ran, whether one raised, or whether --selftest-captions' own sys.exit(1)
# fired on the way out -- finally still runs before a SystemExit propagates.
_LIVE_VSD_SESSIONS: "list[Session]" = []


def flush_live_vsd_sessions() -> None:
    """Flush and forget every Session on the module's live-VSD registry -- call once,
    from a try/finally wrapped around a script's WHOLE dispatch (dgb_shots.main(),
    this file's own main()). A no-op if nothing attached --vsd this run."""
    global _LIVE_VSD_SESSIONS
    for s in _LIVE_VSD_SESSIONS:
        s.vsd_flush()
    _LIVE_VSD_SESSIONS = []


_vsd_img_bin_cache: "Path | None" = None
_vsd_img_build_failed: "str | None" = None


def _vsd_img_bin() -> Path:
    """Compile tools/vsd_img.c once per process -- design S4.6's own recipe, the same
    host-`cc` posture tools/gb_claims.py's _gb_driver() already uses for its own driver
    binary. Session.vsd_report() is the only caller. Raises vsd.VsdError (never a silent
    skip, golden rule 3) if this host has no C compiler or the build fails."""
    global _vsd_img_bin_cache, _vsd_img_build_failed
    if _vsd_img_bin_cache is not None:
        return _vsd_img_bin_cache
    if _vsd_img_build_failed is not None:
        raise vsd.VsdError(_vsd_img_build_failed)
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if not cc:
        _vsd_img_build_failed = "vsd_report(): no host C compiler (cc/gcc/clang) found"
        raise vsd.VsdError(_vsd_img_build_failed)
    bin_dir = Path(tempfile.mkdtemp(prefix="vsd_img_bin_"))
    binp = bin_dir / "vsd_img"
    cmd = [cc, "-std=c11", "-DFF_USE_MKFS=1", "-Dsiprintf=sprintf",
           "-I", str(ROOT / "tests" / "hostfat"), "-I", str(ROOT / "lib" / "fatfs"),
           "-I", str(ROOT / "source"), "-I", str(ROOT / "tools"),
           str(ROOT / "tools" / "vsd_img.c"), str(ROOT / "tools" / "host_walk.c"),
           str(ROOT / "source" / "pdna_romver.c"),
           str(ROOT / "lib" / "fatfs" / "ff.c"), str(ROOT / "lib" / "fatfs" / "ffunicode.c"),
           str(ROOT / "tests" / "hostfat" / "ramdisk.c"),
           "-o", str(binp)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0 or not binp.is_file():
        _vsd_img_build_failed = f"vsd_report(): vsd_img build failed: {r.stderr.strip()[-2000:]}"
        raise vsd.VsdError(_vsd_img_build_failed)
    _vsd_img_bin_cache = binp
    return _vsd_img_bin_cache


def load_mgba():
    vendor = ROOT.parent / "rec2mp4" / "vendor"
    if not vendor.is_dir():
        sys.exit(f"vendor path missing: {vendor} (docs/HANDOFF.md's mGBA harness recipe)")
    sys.path.insert(0, str(vendor))
    import mgba.core, mgba.image, mgba.log   # noqa: E402
    mgba.log.silence()                        # MANDATORY: else per-instruction logs flood stderr
    return mgba.core, mgba.image


class Session:
    """One booted core + the tap/shot primitives every shot list below is built from."""

    def __init__(self, core_mod, image_mod, rom_path: Path, out_dir: Path, prefix: str,
                 *, vsd_img: "Path | None" = None, vsd_knobs: "dict | None" = None):
        self.core = core_mod.load_path(str(rom_path))
        if self.core is None:
            sys.exit(f"mgba could not load {rom_path}")
        self.screen = image_mod.Image(*self.core.desired_video_dimensions())
        self.core.set_video_buffer(self.screen)   # BEFORE reset()
        self.core.reset()
        self.rom_path = Path(rom_path)   # BACKLOG #214: the FUSED .gba image this Session
                                          # booted -- claim_gb= extracts the embedded GB
                                          # ROM out of THIS file (see _gb_rom_file()), never
                                          # a separately-supplied corpus ROM that might not
                                          # be the exact bytes the app actually rendered from.
        self.out_dir = out_dir
        self.prefix = prefix
        self.taken = []     # (filename, caption, claim_info) -- BACKLOG #184: claim_info is a
                             # dict (possibly empty) merged straight into the manifest entry --
                             # "claim"/"claim_absent" (the original strings, for an offline
                             # re-check) and "claim_failed" (this capture's own failures, if any)
        self.skipped = []   # (label, reason)
        self.any_claim_failed = False   # BACKLOG #184: sticky across every shot() this Session
                                         # takes -- main()'s exit status reads this, not a per-shot
                                         # return value, so no CLI dispatch branch needs editing.
        self._last_shot = None    # (name, raw RGB bytes) -- for the consecutive-differ check
        self._gb_rom_file_cache: Path | None = None   # BACKLOG #214: lazy, memoized per Session

        # BACKLOG #179 Phase A step A3: the virtual SD. `vsd_img` (explicit) wins over
        # set_default_vsd()'s CLI-wide default; both absent (every runner today, and
        # any runner run without --vsd) leaves self.vsd None and run()/tap() below are
        # exactly their pre-A3 selves -- no mgba call, no extra frame cost.
        self.vsd: "vsd.VsdServer | None" = None
        self._vsd_pre_path: "Path | None" = None   # BACKLOG #179 A3 review D1: the
                                                     # <img>.pre snapshot vsd_snapshot()
                                                     # takes below, vsd_report()'s "before"
        chosen_img = vsd_img if vsd_img is not None else _DEFAULT_VSD_IMG
        if chosen_img is not None:
            chosen_knobs = vsd_knobs if vsd_knobs is not None else _DEFAULT_VSD_KNOBS
            rom_bytes = self.rom_path.read_bytes()
            self.vsd = vsd.attach_server(self.core, rom_bytes, Path(chosen_img), **chosen_knobs)
            self.vsd_snapshot()          # BACKLOG #179 A3 review D1: the "before" side of
                                          # a chain's diff, taken at attach, before a single
                                          # frame of this run has been served.
            _LIVE_VSD_SESSIONS.append(self)   # flushed by flush_live_vsd_sessions() in a
                                               # try/finally around the caller's dispatch

        self.run(180)       # let the boot screen (info page) fully settle; also where
                             # vsd_attach()'s 4-frame handshake gets served, if attached

    def _gb_rom_file(self) -> Path:
        """BACKLOG #214 item 2: the embedded GB ROM (TYPE_ROM_GEN1/2) this Session's own
        fused image carries, extracted to a temp file ONCE (memoized) so claim_gb='s
        matcher reads the EXACT bytes gbscr_text()/rom_gbui_glyph() rendered from on this
        run -- never a separately-supplied corpus ROM that might be a different dump.
        Raises ValueError (a setup error, never a silent claim result) if the image has
        zero or more than one embedded ROM entry -- claim_gb= is only meaningful against
        a single-ROM fused image (the same "must be a ONE-ROM fused image" contract
        run_b89_hof()/run_b194_hof()'s own docstrings already state)."""
        if self._gb_rom_file_cache is not None:
            return self._gb_rom_file_cache
        blob = self.rom_path.read_bytes()
        rec_off = fuse_gb.locate_record_permissive(blob, str(self.rom_path))
        dir_off, dir_size = fuse_gb.read_record(blob, rec_off)
        if not dir_size:
            raise ValueError(f"claim_gb=: {self.rom_path} has no fused GB directory at all "
                              "(this Session's image was never fused with a ROM)")
        entries = fuse_gb.parse_directory(blob, dir_off, dir_size)
        rom_entries = [e for e in entries if e["type"] in (fuse_gb.TYPE_ROM_GEN1, fuse_gb.TYPE_ROM_GEN2)]
        if len(rom_entries) != 1:
            raise ValueError(f"claim_gb=: {self.rom_path} carries {len(rom_entries)} embedded "
                              "GB ROM(s), expected exactly 1 (a single-ROM fused image)")
        e = rom_entries[0]
        rom_bytes = blob[e["offset"]:e["offset"] + e["size"]]
        tmp_dir = Path(tempfile.mkdtemp(prefix="gb_shots_claimgb_"))
        ext = ".gbc" if e["type"] == fuse_gb.TYPE_ROM_GEN2 else ".gb"
        tmp_path = tmp_dir / f"embedded{ext}"
        tmp_path.write_bytes(rom_bytes)
        self._gb_rom_file_cache = tmp_path
        return tmp_path

    def run(self, n: int) -> None:
        """The single funnel every frame in both gb_shots.py and dgb_shots.py is meant
        to advance the core through (S4.5 -- dgb_shots.py constructs Session objects
        directly and is meant to never advance the core any other way). That
        "never" was not enforced anywhere until BACKLOG #179 A3 review D2: two
        long-poll loops in tools/dgb_shots.py called `s.core.run_frame()` directly,
        bypassing this method's own vsd.service() call -- an in-flight VSD request
        then timed out mid-poll and the eventual stale reply landed in an abandoned
        buffer. Both are fixed (now call `s.run(1)`) and
        tests/host_vsd_funnel_sites_test.py greps tools/*.py for any remaining
        `\\.core\\.run_frame(` outside this file, so a regression fails the host
        suite instead of silently reintroducing the bypass.

        When self.vsd is attached, service() is called once per frame, AFTER
        run_frame() so the emulated CPU is stopped for the whole call (S4.4 -- no
        barriers needed for that reason). Unattached, this branch is never taken:
        byte-identical to every runner's pre-A3 timing (S4.3's own gate)."""
        for _ in range(n):
            self.core.run_frame()
            if self.vsd is not None:
                self.vsd.service()

    def vsd_flush(self) -> None:
        """BACKLOG #179 A3 review D1 (BLOCKER): VsdImage.flush() had no caller anywhere
        in this tree -- every --vsd write lived only in the server's in-process
        bytearray and died with the process the instant it exited, so nothing a chain
        wrote was ever actually on disk for a later `vsd_img list`/`vsd_diff` to see.
        No-op unless this Session attached --vsd."""
        if self.vsd is not None:
            self.vsd.image.flush()

    def vsd_snapshot(self) -> None:
        """S4.6/A3's own per-chain diff hook, "before" half: copy the just-attached
        image file to `<img>.pre` -- vsd_report()'s own comparison point. Called once,
        automatically, from __init__ right after attach (before this Session has run a
        single frame), so a runner never has to remember to call it itself. No-op
        unless this Session attached --vsd."""
        if self.vsd is None:
            return
        self.vsd_flush()   # BACKLOG #174/#175 review D7: this copied the ON-DISK
                           # image without flushing the in-process bytearray to it
                           # first -- two snapshots with no vsd_report() between them
                           # silently discarded whatever was written in between.
                           # Idempotent and a no-op without --vsd, so no existing
                           # chain's diff changes.
        pre = Path(str(self.vsd.image.path) + ".pre")
        shutil.copyfile(self.vsd.image.path, pre)
        self._vsd_pre_path = pre

    def vsd_report(self, expect_changed: "list[str] | None" = None) -> "set[str]":
        """S4.6's own per-chain diff hook, "after" half: flush the live image, run
        tools/vsd_img.c's `list` mode against BOTH the `<img>.pre` snapshot
        vsd_snapshot() took at attach and the just-flushed live image, diff them with
        tools/vsd_diff.py's own parser (never a second implementation), print the
        result, and return the union of added|changed paths.

        If `expect_changed` is given, a mismatch is a loud, mechanical failure --
        BACKLOG #184's own contract for a caption-shaped assertion (claim=): print
        [VSD DIFF FAILED] and exit the process with status 1, rather than let a runner
        silently claim success while writing the wrong files (or nothing).

        Raises vsd.VsdError if this Session never attached --vsd (nothing to diff) --
        a runner calling this without --vsd is a setup error, not a skip."""
        if self.vsd is None or self._vsd_pre_path is None:
            raise vsd.VsdError(
                f"{self.prefix}vsd_report(): no attached --vsd session -- nothing to diff")
        self.vsd_flush()
        binp = _vsd_img_bin()
        tmp_dir = Path(tempfile.mkdtemp(prefix="vsd_report_"))
        before_path = tmp_dir / "before.list"
        after_path = tmp_dir / "after.list"
        before_r = subprocess.run([str(binp), "list", str(self._vsd_pre_path)],
                                   capture_output=True, text=True)
        if before_r.returncode != 0:
            raise vsd.VsdError(f"vsd_report(): vsd_img list {self._vsd_pre_path} failed: "
                                f"{before_r.stderr.strip()}")
        after_r = subprocess.run([str(binp), "list", str(self.vsd.image.path)],
                                  capture_output=True, text=True)
        if after_r.returncode != 0:
            raise vsd.VsdError(f"vsd_report(): vsd_img list {self.vsd.image.path} failed: "
                                f"{after_r.stderr.strip()}")
        before_path.write_text(before_r.stdout, encoding="ascii")
        after_path.write_text(after_r.stdout, encoding="ascii")
        before = vsd_diff.parse_list(str(before_path))
        after = vsd_diff.parse_list(str(after_path))
        added, removed, changed = vsd_diff.diff_lists(before, after)
        for p in added:
            print(f"  [VSD] + {p} {after[p].size} {after[p].crc}")
        for p in removed:
            print(f"  [VSD] - {p} {before[p].size} {before[p].crc}")
        for p in changed:
            print(f"  [VSD] ~ {p} {before[p].size} {before[p].crc} -> {after[p].size} {after[p].crc}")
        changed_set = set(added) | set(changed)
        if expect_changed is not None:
            want = set(expect_changed)
            if changed_set != want:
                print(f"[VSD DIFF FAILED] {self.prefix}: expected changed={sorted(want)} "
                      f"got={sorted(changed_set)} (added={added} removed={removed} "
                      f"changed={changed})", file=sys.stderr)
                sys.exit(1)
        return changed_set

    def _io_quiesce(self) -> None:
        """S4.5: after tap()'s own settle, keep running extra frames while the VSD is
        still being served, until VSD_QUIESCE_IDLE consecutive frames produce no
        transaction, capped at VSD_QUIESCE_CAP frames total. "Is the I/O finished?"
        becomes OBSERVED rather than guessed at -- a chain that hits the cap is a loud
        failure (I/O never quiesced), not a screenshot of a busy panel (design S4.5).
        Unattended (self.vsd is None) this is a no-op: no request is ever served, so
        the while loop's own guard below never even starts it."""
        if self.vsd is None:
            return
        idle = 0
        frames = 0
        while idle < VSD_QUIESCE_IDLE and frames < VSD_QUIESCE_CAP:
            self.core.run_frame()
            served = self.vsd.service()
            frames += 1
            idle = 0 if served else idle + 1
        if frames >= VSD_QUIESCE_CAP and idle < VSD_QUIESCE_IDLE:
            raise RuntimeError(
                f"{self.prefix}: VSD I/O never quiesced after {frames} frames "
                f"(S4.5's own {VSD_QUIESCE_CAP}-frame cap) -- treat as a real failure")

    def tap(self, *names: str, settle: int = SETTLE) -> None:
        """A single key-down edge. Multiple names are pressed as ONE simultaneous chord
        (A+B together), NOT sequential presses -- ORing the same name twice collapses to
        one edge, which is exactly the bug that sent an intended double-DOWN into a
        single row move on the first pass of this script (07/08 opened the wrong menu
        row as a result). Use press_n() for "the same key, N times in a row"."""
        mask = 0
        for n in names:
            mask |= KEY[n]
        self.core.set_keys(raw=mask)
        self.run(HOLD)
        self.core.set_keys(raw=0)
        self.run(settle)
        self._io_quiesce()   # S4.5 -- no-op unless self.vsd is attached

    def press_n(self, name: str, n: int, settle: int = SETTLE) -> None:
        for _ in range(n):
            self.tap(name, settle=settle)

    def shot(self, name: str, caption: str, settle: int = 0, allow_same: bool = False,
              claim: "str | list[str] | None" = None,
              claim_absent: "str | list[str] | None" = None,
              claim_gb: "str | list[str] | None" = None) -> Path:
        # allow_same: the caller KNOWS this frame is expected to equal the previous
        # shot (e.g. the same refusal dialog reached by a different input) and says
        # so in the caption; the identical-frame guard below is then skipped.
        #
        # claim/claim_absent (BACKLOG #184): a caption is a CLAIM about pixels -- these
        # make the claim mechanical. Every string in `claim` must be found on the saved
        # frame (gb_claims.find(), tried against BOTH PokeDNA fonts unless the caller
        # narrows it -- see gb_claims.py); every string in `claim_absent` must NOT be.
        # A failure never blocks the PNG/manifest entry from being written (the frame is
        # still evidence, possibly of a real bug) -- it is recorded on the entry as
        # `claim_failed` and printed loudly; Session.any_claim_failed then makes the
        # RUN's exit status non-zero (main() checks it once at the end).
        #
        # claim_gb (BACKLOG #214 item 2): the SAME contract as claim=, for text drawn
        # by the GB-shell's own composited tile font (gbscr_text(), the Hall of
        # Fame/trainer-card screens) -- gb_claims.check() cannot read that font at all
        # (see gb_claims.py's claim_gb= design comment), so this goes through
        # gb_claims.check_gb() against the GB ROM embedded in THIS Session's own fused
        # image (_gb_rom_file()) instead.
        if settle:
            self.run(settle)
        img = self.screen.to_pil().convert("RGB")
        path = self.out_dir / f"{self.prefix}{name}.png"

        # Fail LOUDLY here rather than write a plausible-but-wrong PNG a human has to
        # catch by eye later (exactly the class of bug the DV-edit and MOVE-TO/RELEASE
        # navigation mistakes earlier in this script's own history were: a tap silently
        # not landing, leaving the emulator on the previous screen).
        lo, hi = img.convert("L").getextrema()
        if lo == hi:
            raise RuntimeError(
                f"{self.prefix}{name}: captured frame is a single flat colour "
                f"(value {lo}) -- the emulator is very unlikely to be showing a real "
                f"screen; a settle/press was probably too short or landed mid-transition.")
        raw = img.tobytes()
        if not allow_same and self._last_shot is not None and raw == self._last_shot[1]:
            raise RuntimeError(
                f"{self.prefix}{name}: pixel-identical to the previous shot "
                f"({self._last_shot[0]}) -- the tap(s) between them had no visible "
                f"effect, so this is very likely the same screen twice, not two "
                f"different ones (see this file's own DOWN,DOWN chord-vs-sequence bug).")
        self._last_shot = (name, raw)

        img.save(path)

        # claim_info becomes the manifest entry's EXTRA keys (merged in verbatim by the
        # manifest writer below): the original claim/claim_absent strings are kept, not
        # just this capture's pass/fail, so --selftest-captions can independently
        # RE-DERIVE pass/fail from the PNG later (offline, no mGBA) instead of trusting
        # a stale claim_failed nobody re-ran.
        claim_info: dict = {}
        if claim is not None:
            claim_info["claim"] = claim
        if claim_absent is not None:
            claim_info["claim_absent"] = claim_absent
        if claim is not None or claim_absent is not None:
            claim_failed = gb_claims.check(img, claim=claim, claim_absent=claim_absent)
            if claim_failed:
                claim_info["claim_failed"] = claim_failed
                self.any_claim_failed = True
                for f in claim_failed:
                    print(f"  [CLAIM FAILED] {path.name}: {f}")

        if claim_gb is not None:
            claim_info["claim_gb"] = claim_gb
            # BACKLOG #214: the fused image path is recorded too -- not for this
            # live check (which already has self.rom_path in hand) but so a future
            # offline re-check (item 5's own scope note) can re-extract the exact
            # same embedded ROM from a STABLE path, rather than a temp file that
            # dies with this process.
            claim_info["claim_gb_image"] = str(self.rom_path)
            claim_gb_failed = gb_claims.check_gb(img, self._gb_rom_file(), claim_gb=claim_gb)
            if claim_gb_failed:
                existing = claim_info.get("claim_failed", [])
                claim_info["claim_failed"] = existing + claim_gb_failed
                self.any_claim_failed = True
                for f in claim_gb_failed:
                    print(f"  [CLAIM FAILED] {path.name}: {f}")

        self.taken.append((path.name, caption, claim_info))
        print(f"  [ok]   {path.name:32s} {caption}")
        return path

    def skip(self, name: str, reason: str) -> None:
        self.skipped.append((name, reason))
        print(f"  [skip] {name:32s} {reason}")


def run_gold(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "gold_")
    print("== Gold.sav (Gen 2, --clip seeded) ==")

    # Occupied-mon menu row order (source/pdna_main.c:4565-4605, app_mon_menu_readonly),
    # for a Gen-2 mount with every hook present and the slot not locked (k_gb_ops_gen2,
    # source/pdna_gen12.c:3295-3300 -- this table has .item set, unlike Gen 1's):
    #   0:VIEW/EDIT  1:ITEM  2:LEGALITY  3:MOVE TO BOX  4:COPY  5:DUPLICATE
    #   6:TO DAY-CARE  7:EXPORT .pk  8:RELEASE  9:CANCEL
    # BACKLOG #149 (this lane, re-derived from a live per-tap trace, GB_SHOTS_TRACE=1):
    # `sel` is declared `int sel = 0, top = 0;` LOCAL to app_mon_menu_readonly
    # (pdna_main.c:4624) -- the cursor does NOT persist between openings, it resets to
    # row 0 every time the menu opens. And every row's own case in the dispatch switch
    # (pdna_main.c:4649-4667) ends with `return <hook call or false>` -- picking ANY row
    # and then backing out of whatever it opened (even by cancelling) closes the WHOLE
    # menu, straight back to the box grid; there is no "back to the row list" state.
    # Both assumptions in the previous two lane commits (4732830/288a067 -- cursor
    # "stays" at the last row, cancelling a sub-picker returns to the row list) were
    # wrong; every row demoed below is a fresh box-grid-A-DOWN*n-A sequence.
    #
    # A trace also caught a THIRD surprise the old script never accounted for: B from
    # the box grid does not go straight to the info page -- it first opens a "NOT
    # TRANSFERABLE" interstitial (why the converted-copy mons that don't fit stay in
    # their boxes), and only a second B (or A) from there reaches the info page. Not
    # used below (every reopen goes box-grid -> A, never through the info page), but
    # worth knowing if a future shot needs to leave the box grid.
    s.shot("01_info", "S1: the info page — Gold/Silver save, converted-copy notice, counts")

    s.tap("A", settle=BIG_SETTLE)          # info -> box grid
    s.shot("02_box_grid", "S2: box grid — GB BOX1 20/20 (BACKLOG #40(a): the banner now uses "
                          "the source's own capacity, not the Gen-3 grid's 30 cells)")

    s.tap("A", settle=BIG_SETTLE)          # A on slot 0 (Bulbasaur) -> mon menu, row 0 selected
    s.shot("03_mon_menu", "S2: the read-only mon menu — Gen-3 parity (BACKLOG #42/#43 batch, "
                          "2026-09-05): VIEW and EDIT are now ONE row (\"VIEW / EDIT\", same "
                          "label the Gen-3 menu uses), then ITEM/LEGALITY/MOVE TO BOX/COPY/"
                          "DUPLICATE/TO DAY-CARE/EXPORT .pk/RELEASE (UX-parity audit, "
                          "2026-09-07: reordered + MOVE TO relabelled MOVE TO BOX to match "
                          "Gen 3's own row order/labels exactly)")

    # ---- MOVE TO BOX: row 3. Fresh menu, DOWN x3 from row 0. ----
    s.press_n("DOWN", 3)                   # row 0 (VIEW/EDIT) -> row 3 (MOVE TO BOX)
    s.tap("A", settle=BIG_SETTLE)          # open the box/party picker
    s.shot("07_move_to_picker", "S3: MOVE TO BOX — the destination box/party picker "
                                 "(UX-parity audit: relabelled from \"MOVE TO\" to Gen 3's own "
                                 "\"MOVE TO BOX\", and moved to sit after LEGALITY, matching "
                                 "Gen 3's own row order)")
    s.tap("B", settle=BIG_SETTLE)          # cancel picker -> the WHOLE menu closes -> box grid
                                            # directly (RO_MOVE returns the hook's result straight
                                            # out of app_mon_menu_readonly; there is no "back to
                                            # the row list" -- verified by trace, BACKLOG #149)

    # ---- RELEASE: row 8. Reopen the menu fresh (cursor always resets to row 0), DOWN x8. ----
    s.tap("A", settle=BIG_SETTLE)          # box grid -> mon menu, row 0 again
    s.press_n("DOWN", 8)                   # row 0 (VIEW/EDIT) -> row 8 (RELEASE)
    s.tap("A", settle=BIG_SETTLE)
    s.shot("08_release_confirm", "S3: RELEASE — the confirm popup (UX-parity audit: RELEASE "
                                  "is now the LAST row before CANCEL, matching Gen 3's own "
                                  "occupied-mon-menu order, not 2nd)")
    s.tap("B", settle=BIG_SETTLE)          # cancel confirm -> the menu closes -> box grid directly

    # ---- BACKLOG #41: VIEW/EDIT summary. Reopen the menu fresh (row 0 = VIEW/EDIT already
    # selected), one A both opens the menu, the next A picks VIEW/EDIT. ----
    s.tap("A", settle=BIG_SETTLE)          # box grid -> mon menu, row 0 (VIEW/EDIT)
    s.tap("A", settle=BIG_SETTLE)          # A on VIEW/EDIT -> the summary, VIEW mode, card 0
    s.run(BIG_SETTLE)                      # extra settle for summary entry animation
    s.shot("04_view_info", "BACKLOG #41: the native summary, VIEW, card 0 INFO — "
                            "nickname/level/type/OT/item/friendship/EXP")
    s.tap("R", settle=BIG_SETTLE)          # card 0 -> 1 (SKILLS)
    s.shot("04b_view_skills", "BACKLOG #41 slice E1: VIEW, card 1 SKILLS — HP/Atk/Def/Spe/"
                               "SpA/SpD, each a two-row cell (value, then DV + stat exp)")
    s.tap("R", settle=BIG_SETTLE)          # card 1 -> 2
    s.shot("04c_view_moves", "BACKLOG #41: VIEW, card 2 MOVES — 4 moves, PP cur/max, PP Ups")
    s.tap("R", settle=BIG_SETTLE)          # card 2 -> 3
    s.shot("04d_view_origin", "BACKLOG #41 slice E1: VIEW, card 3 ORIGIN (new) — the honest "
                               "\"Gen 3 preview only / edits change the GB save\" story, plus "
                               "the sidecar link status")
    s.tap("L", settle=BIG_SETTLE)          # card 3 -> back to 2, so B below leaves from MOVES
                                            # (matches the pre-E1 script's own exit point)
    s.tap("B", settle=BIG_SETTLE)          # leave (not dirty -> no confirm) -> box grid

    # ---- EDIT: Gen-3 parity (BACKLOG #42/#43 batch) retired the standalone EDIT row that
    # opened straight into edit mode -- the merged VIEW/EDIT row always opens in VIEW, and
    # (like the Gen-3 summary) A INSIDE it flips to edit mode (pdna_gbsummary.c's own KEY_A
    # handler, c->can_edit). Fresh menu -> VIEW/EDIT (row 0, already selected) -> A again
    # for VIEW -> A again for edit. ----
    s.tap("A", settle=BIG_SETTLE)          # box grid -> mon menu, row 0
    s.tap("A", settle=BIG_SETTLE)          # A on VIEW/EDIT -> the summary, VIEW
    s.tap("A", settle=BIG_SETTLE)          # A inside VIEW -> editing = true, same as Gen 3
    s.shot("05_edit_info", "Gen-3 parity: A inside the VIEW/EDIT summary flips to edit "
                            "mode, card 0 — the frame on Nickname (fsel resets on entry)")

    s.tap("R", settle=BIG_SETTLE)          # card 0 -> 1 (SKILLS); fsel resets to 0 (HP's SE)
    s.tap("DOWN", settle=SETTLE)           # fsel 0 (SE0/HP) -> 1 (DV Atk) — card_skills()'s
                                            # own registration order, stat_row() called
                                            # HP/ATK/DEF/SPE/SpA/SpD in that order and each
                                            # registers DV-then-SE, so index 1 is ATK's DV.
    s.shot("05b_edit_skills_dv_atk", "BACKLOG #41 slice E1: EDIT, card 1 SKILLS, the moving "
                                      "outline (not an inline highlight any more) on DV Atk")
    # This Bulbasaur's DV Atk starts at 15 (max). pdna_gbsummary reserves L/R for CARD
    # FLIP (Gen-3 pdna_inspect's own convention — BACKLOG #41 asked for "the same feel
    # as gen 3"), so there is no big-step shoulder adjust here any more (SELECT still
    # reaches pdna_gbedit.c's flat list, which keeps it) — plain LEFT taps, one per DV
    # point, the same [0,15] clamp gb_editor.c's dv_stat enforces. Not every tap lands
    # as a discrete -1 once mGBA's own key-repeat kicks in over this many presses (a
    # measured run stopped at DV 4, not 0) — the DEMONSTRATION this shot exists for is
    # the live header flip, which needs "low enough", not exactly zero.
    for _ in range(15):
        s.tap("LEFT", settle=6)
    s.shot("06_edit_dv_changed", "BACKLOG #41: DV Atk drops low enough to flip the header "
                                  "M -> F live (Gen-2 gender-from-DV), same as the old flat editor")

    s.tap("B", settle=BIG_SETTLE)          # edit mode -> VIEW (edits kept, gbedit_confirm not shown yet)
    s.tap("B", settle=BIG_SETTLE)          # leave the mon; dirty -> gbedit_confirm() (shared with pdna_gbedit.c)
    s.shot("07_confirm", "BACKLOG #41: leaving with unsaved edits — the SAME gbedit_confirm() "
                          "panel pdna_gbedit.c uses (shared helper, not a second copy)")
    s.tap("A", settle=BIG_SETTLE)          # A = write -> gb_edit_commit -> gb_persist refuses (no SD in mGBA)
    s.shot("07b_save_refusal", "BACKLOG #41: confirmed -> gb_persist refuses (no SD card in "
                                "mGBA) — honest evidence, not a bug")
    s.tap("A", settle=BIG_SETTLE)          # A ("Press A") dismisses the refusal -> box grid
                                            # directly -- NOT B (BACKLOG #149: this screen's own
                                            # footer reads "Press A"; B does nothing here)

    return s


def run_gold_gender(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #51: the Gender row, a fresh boot independent of run_gold()'s own in-place
    DV edit above (same reasoning as run_gold_paste's own separate boot). Bulbasaur
    (box slot 0) has a real, non-fixed gender ratio -- 0x1F, 12.5% female (female =
    Atk DV <= 1), NOT 0x7F/50-50 (G1 review LOW-2, 2026-09-08: this docstring and the
    caption below both had the wrong ratio) — gbe_has_gender_row() shows the row for
    exactly this reason (source/gb_editor.c)."""
    s = Session(core_mod, image_mod, rom, out_dir, "gold_")
    print("== Gold.sav (Gen 2) -- BACKLOG #51 Gender row ==")
    s.tap("A", settle=BIG_SETTLE)          # info -> box grid
    s.tap("A", settle=BIG_SETTLE)          # slot 0 (Bulbasaur) -> mon menu
    s.tap("A", settle=BIG_SETTLE)          # VIEW/EDIT -> the summary, VIEW, card 0 INFO
    s.tap("A", settle=BIG_SETTLE)          # A inside VIEW -> editing = true
    s.press_n("DOWN", 2)                   # Name -> Lv -> Gender (card_info's own row order,
                                            # source/pdna_gbsummary.c: NICK, LEVEL, GENDER, OT...)
    s.shot("13_gender_before", "#51: the Gender row, field cursor on it — Bulbasaur reads "
                                "M (Atk DV 15 under this save's own ratio 0x1F, 12.5% female)")
    s.tap("A", settle=BIG_SETTLE)          # gbe_flip_gender: nearest Atk DV of the other gender
    s.shot("13b_gender_after", "#51: A flips it — Gender now reads F, and the shared left "
                                "panel's header (name/level/gender chip) updates live in the "
                                "SAME repaint, same as the old DV-edit demo (06_edit_dv_changed) "
                                "used to show indirectly")
    s.tap("B", settle=BIG_SETTLE)          # edit -> view (edits kept, not yet asked to write)
    s.tap("B", settle=BIG_SETTLE)          # leave; dirty -> gbedit_confirm (not shot here, see 07_confirm)
    s.tap("B", settle=BIG_SETTLE)          # discard, whichever confirm/summary layer this landed on
    return s


def run_gold_create(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #50, re-shot 2026-09-07/08 for the UX-parity rewrite (Guy: "Pokemon
    creation is not from the Pokedex view -- fix that"): CREATE on an empty cell now
    opens the SAME pdna_pick.c pick_species() the Gen-3 create flow uses (icon grid,
    filters, search), restricted to this session's own generation
    (pick_species_set_max_dex) -- replacing the old bespoke "No.NNN NAME" text list
    (gb_create_pick_species, deleted). There is also no level PROMPT any more:
    rom_gblearn_min_level() computes the species' own lowest legal level off the SAME
    ROM lookup, mirroring gen3_build_level's "5 for a Bulbasaur, 36 for a Charizard".

    `rom` is the SAME clip-bearing --gold image run_gold()/run_gold_paste() use
    (this module's own docstring: "+ --clip, an 80-byte real Gen-3 box record") --
    the empty-cell menu shot below (14b) therefore shows PASTE HERE alongside
    CREATE, which is the point: BLOCKING-1's fix (G1 review, 2026-09-08) means
    that row is now gated on the SAME clip actually being present.

    GB BOX1 (the box the info page always opens into) is 20/20 on this corpus, same
    as every box run_gold() already notes — box index 13 ("GB BOX14" on screen, BACKLOG #251 re-derivation: 1-indexed display of a 0-indexed box) has
    real room, 17/20, discovered the same way tools/gb_retail_gate.py's
    first_room_box() does. This is ALSO the run that caught the box-resolution bug
    fixed before this file existed in its current form: an EARLIER version of
    gb_create_hook trusted app_mon_menu's own (box, slot) parameters, which
    pdna_box.c hard-codes to (0, cur) for every is_bank source regardless of which box
    is actually on screen — CREATE from box 13 kept refusing "BOX FULL" against box
    0's real 20/20, caught by hand on exactly this navigation before the fix
    (Gb12Mount.ui_box) landed. The species picker is NOT pure UI with no file I/O
    (G1 review LOW-7, 2026-09-08: an earlier draft of this docstring claimed it was)
    -- pick_species()'s own icon draw (mon_icon_for -> icon_store) DOES read the SD
    card in a full-art build; it is only the ARTLESS build under test here that
    never touches it (mon_icon_for returns 0 immediately, pick_species's own "art-
    free -> text list" fallback, pdna_pick.c). gb_create_learn()'s own ROM lookup
    (level + moveset, one open) is the ROM-lookup refusal captured below regardless
    of which build this runs against — the SAME "SD-backed action refuses in the
    emulator, captured as evidence" shape 07b_save_refusal/10b_paste_refusal
    already use."""
    s = Session(core_mod, image_mod, rom, out_dir, "gold_")
    print("== Gold.sav (Gen 2) -- BACKLOG #50 CREATE (UX-parity re-shoot) ==")
    s.tap("A", settle=BIG_SETTLE)          # info -> box grid (GB BOX1, 20/20)
    # BACKLOG #251 shots-refresh fix: 13 R presses at plain SETTLE dropped some of
    # them (each R re-pages the WHOLE box, a bigger repaint than a cursor move) --
    # reproduced live: this landed on "BOX10" (still 20/20, still occupied), not
    # "BOX14" (17/20, real room), and the DOWN/RIGHT below then walked onto another
    # occupied cell instead of the empty one this shot claims. BIG_SETTLE between
    # each R press lands correctly and reproducibly (verified by hand, several runs).
    s.press_n("R", 13, settle=BIG_SETTLE)  # -> GB BOX14 on screen (17/20, real room)
    s.press_n("DOWN", 2)
    s.press_n("RIGHT", 5)                  # cursor -> slot 17, the first real empty slot
    s.shot("14_cursor_on_empty", "#50: cursor on a real empty slot (GB BOX14, 17/20)")

    s.tap("A", settle=BIG_SETTLE)          # open the empty-cell menu
    # G1 review BLOCKING-1 (2026-09-08): this row list used to add PASTE
    # UNCONDITIONALLY (app_mon_menu_readonly's own empty branch never re-checked
    # g_src_ops->paste/g_clip.occupied/!g_clip.from_gb once entry was ALSO
    # allowed by create_ok alone) -- a Gen-2 COPY (native, from_gb) into the
    # clipboard, then PASTE on a DIFFERENT empty cell, would have round-tripped
    # a lossy duplicate back down. Fixed to re-check the same three terms Gen 3's
    # own empty branch does. --gold's own fused image carries a REAL Gen-3 clip
    # (an 80-byte box record fused via --clip, tools/extract_gen3_record.c --
    # see this file's own module docstring), so PASTE HERE correctly appears
    # here (Gen-3's own label, not the deleted PDNA_LBL_PASTE_GB) -- run against
    # a clip-LESS fused image, this exact same menu correctly shows CREATE and
    # CANCEL only (verified separately: an ad-hoc no-clip run during this
    # review response, not re-shot into the canonical set -- rebuilding it needs
    # a second fused image this script's own --gold/--red pair has no slot for).
    s.shot("14b_empty_menu", "#50: the empty-cell menu -- CREATE, then PASTE HERE (BLOCKING-1: "
                              "now correctly gated on g_clip.occupied && !g_clip.from_gb, same as "
                              "Gen 3's own empty branch; used to show unconditionally), then "
                              "CANCEL -- CREATE first, matching Gen-3's own empty-cell menu order")

    s.tap("A", settle=BIG_SETTLE)          # select CREATE -> pick_species(1), restricted
    s.shot("14c_species_picker", "#50: UX-parity -- CREATE now opens the SAME pick_species() "
                                  "screen Gen-3's own app_create_mon uses -- Bulbasaur (dex 1) "
                                  "pre-selected, header shows the type badge + the restricted "
                                  "count (pick_species_set_max_dex(251) for a Gen-2 session)")

    # UX-parity restriction demo: R cycles the filter -- Gen 3+ is skipped outright
    # (filter_usable(), pdna_pick.c) since this session's own ceiling already excludes
    # it. All -> Gen 1 -> Gen 2 -> Legendary in 3 presses (Gen 3+ never appears).
    s.press_n("R", 3, settle=SETTLE)
    s.shot("14c2_species_filter_skips_gen3", "#50: UX-parity -- 3x R cycles All -> Gen 1 -> "
                                              "Gen 2 -> Legendary -- \"Gen 3\" never appears in "
                                              "a Gen-2-restricted create, unlike the unrestricted "
                                              "Gen-3 picker's own 5-stop cycle")
    s.press_n("L", 2, settle=SETTLE)       # Legendary -> (skip Gen 3) -> Gen 2 -> Gen 1:
                                            # back to a filter that includes Bulbasaur again
    s.shot("14c3_species_filter_back_to_gen1", "#50: UX-parity -- back to the \"Gen 1\" filter "
                                                "(2x L) -- Bulbasaur (dex 1) is selected again, "
                                                "the list's own first entry")

    s.tap("A", settle=BIG_SETTLE)          # pick Bulbasaur -> gb_create_learn (level + moveset)
    s.shot("14d_scratch_no_rom_summary", "#265: picked -> NO choice screen (nothing is registered, so "
                                          "there is no ROM to copy from) and NO refusal any more: Gen 2 "
                                          "builds FROM SCRATCH without touching a card -- the NEW-mon "
                                          "summary opens on a level-1 BULBASAUR (Lv1, EXP 0), built from "
                                          "the in-tree Gen-3 tables (Gen 2's base stats/growth are "
                                          "identical to Gen 3's)",
           claim=["BULBASAUR", "Lv1"])
    s.skip("14e_new_mon_legit_copy",
           "the LEGIT COPY arm (A on the #265 choice screen: real level + learnset off the "
           "registered ROM) needs a registered ROM, which mGBA's fused-save harness has no "
           "flashcart to provide here -- it IS captured on the delta-gb image "
           "(dgb_shots.py --standalone's 09b/10/11 frames, and tools/y3_create_shots.py). "
           "Hardware-only on a real SD-card ROM: docs/HW-TEST-2026-09-05-GB-ARC.md §O.")
    s.skip("14f_mon_in_grid",
           "downstream of 14e (gbs_insert -> the box grid re-paging with the new mon "
           "showing) -- same ROM-needs-a-real-SD-card gap, hardware-only.")
    return s


def run_gold_paste(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """Separate fresh boot for the PASTE (GB) shots: independent of run_gold()'s in-place
    DV edit above, so a failure there can never take these down with it."""
    s = Session(core_mod, image_mod, rom, out_dir, "gold_")
    print("== Gold.sav (Gen 2, --clip) -- PASTE (GB) on an empty cell ==")
    s.tap("A", settle=BIG_SETTLE)          # info -> box grid (GB BOX1, 20/20)

    # BACKLOG #251 shots-refresh re-derivation (was: "Grid is 6 cols x 5 rows; slots
    # 0..19 are filled, 20..29 are the always empty cells... slot 20 = row 3, col 2"):
    # BACKLOG #200 F2 (source/pdna_box.c grid_lr_step(), landed after this was last
    # calibrated) made cells at/after a source's own capacity genuinely UNREACHABLE
    # by cursor movement -- LEFT/RIGHT now skip over any index >= cap and wrap back to
    # the row's own column 0 instead of ever landing on one (blocked_cells()'s own
    # docstring: "cells...don't exist in this game" -- deliberately closing the same
    # "phantom cell past the Game Boy's real capacity" gap #120 S2 F1 closed for the
    # Bank). Reproduced live: DOWN x3 + RIGHT x2 from a fresh BOX1 (20/20) landed back
    # on slot 18 (RATTATA, occupied), not an empty cell -- the whole chain below used
    # to shoot the wrong screen (an occupied-cell mon menu captioned as an empty-cell
    # action menu). BOX1 has no real empty slot at all any more with this corpus (it
    # is genuinely full at capacity), so this now reuses run_gold_create()'s own
    # box13/slot17 navigation (real room, 17/20) instead of chasing a blocked cell.
    s.press_n("R", 13, settle=BIG_SETTLE)  # -> GB BOX14 on screen, 17/20 (real room, same box run_gold_create() uses; BIG_SETTLE per that function's own comment)
    s.press_n("DOWN", 2)
    s.press_n("RIGHT", 5)                  # cursor -> slot 17, the first real empty slot
    s.shot("09_cursor_on_empty_cell", "S5-B: cursor parked on a REAL empty slot (GB BOX14, "
                                       "17/20) before pressing A -- re-derived off run_gold_create()'s "
                                       "own box13 navigation; BOX1's own cells past its 20/20 "
                                       "capacity are no longer cursor-reachable at all (BACKLOG "
                                       "#200 F2's grid_lr_step(), see this function's own comment)")

    s.tap("A", settle=BIG_SETTLE)
    # G1 review BLOCKING-1 (2026-09-08) side effect: this menu now ALSO offers
    # CREATE (every empty GB cell does, box 1's own "always empty" cells 20-29
    # included) -- CREATE, PASTE HERE, CANCEL, not "just PASTE (GB) + CANCEL"
    # as this caption used to claim. CREATE sits FIRST (matching Gen 3's own
    # empty-cell order), so the very next tap below now has to move DOWN once
    # to reach PASTE HERE before pressing A -- an earlier draft of this re-shot
    # skipped that and silently selected CREATE instead, landing on "BOX FULL"
    # (this box is 20/20) rather than the loss screen this function is about;
    # caught by looking at the actual gold_10_loss_screen.png capture, not
    # assumed from the menu's own row count still being small.
    s.shot("09b_paste_gb_popup", "S5-B: an empty cell's action menu — CREATE, PASTE HERE "
                                  "(Gen-3's own label; BLOCKING-1 fix), CANCEL")

    s.press_n("DOWN", 1)                   # CREATE (selected by default) -> PASTE HERE
    s.tap("A", settle=BIG_SETTLE)          # select PASTE HERE -> gen3_to_gb() -> the loss screen
    s.shot("10_loss_screen", "S5-B: the Gen-3-to-GB loss screen (what a real transfer would drop)")

    # BACKLOG #251 shots-refresh re-derivation (was: "GB BOX1 is already at its real
    # Gen-2 capacity (20/20)... the same, first refusal a paste into this box hits"):
    # this whole chain now targets BOX14 (17/20, real room -- see 09_cursor_on_empty_
    # cell's own comment), so the capacity gate no longer fires here at all. Confirmed
    # -> A now reaches the ACTUAL first refusal past a real destination slot: "SIDECAR
    # FOLDER / Nothing transferred." (source/xfer_io.c's own /PokeDNA/xfer sidecar-
    # write attempt, which fails the same way every SD-backed write does in mGBA --
    # no flashcart, so f_open/f_mkdir on the sidecar folder never succeed). Different
    # refusal, same honest-evidence shape as 07b_save_refusal/14d_no_rom_refusal.
    s.tap("A", settle=BIG_SETTLE)
    s.shot("10b_paste_refusal", "S5-B: confirmed -> refused: \"SIDECAR FOLDER / "
                                 "Nothing transferred.\" -- the /PokeDNA/xfer sidecar "
                                 "write (source/xfer_io.c) is itself SD-backed and has "
                                 "no flashcart to write to in mGBA, the same class of "
                                 "honest refusal every other SD-write shot in this file "
                                 "shows (BOX14 has real room, so the capacity gate this "
                                 "caption used to describe never fires here any more)")
    return s


def run_red(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    s = Session(core_mod, image_mod, rom, out_dir, "red_")
    print("== Red.sav (Gen 1, no clip) ==")

    s.tap("A", settle=BIG_SETTLE)
    s.shot("12a_box_grid", "S2: Red.sav box grid — Gen 1")

    # BACKLOG #41: VIEW opens the native summary (card 0 INFO — the requested Gen-1 shot).
    s.tap("A", settle=BIG_SETTLE)          # mon menu
    s.tap("A", settle=BIG_SETTLE)          # VIEW (already selected)
    s.shot("12b_view_info", "BACKLOG #41: Gen-1 native summary, VIEW, card 0 INFO — no "
                             "Item/Friendship rows (Gen-1 has neither)")
    s.press_n("R", 3, settle=BIG_SETTLE)   # INFO -> SKILLS -> MOVES -> ORIGIN
    s.shot("12b2_view_origin", "BACKLOG #41 slice E1: Gen-1, card 3 ORIGIN — the Type row "
                                "reads the record's OWN stored type bytes (g1_to_g3_type()), "
                                "not the species table the shared left panel's chips use")
    s.press_n("L", 3, settle=BIG_SETTLE)   # back to INFO, so the B below leaves from the
                                            # same card the pre-E1 script always did
    s.tap("B", settle=BIG_SETTLE)          # leave -> box grid

    s.tap("A", settle=BIG_SETTLE)          # mon menu
    s.tap("A", settle=BIG_SETTLE)          # VIEW/EDIT (already selected) -> the summary, VIEW
    s.tap("A", settle=BIG_SETTLE)          # A inside VIEW -> editing = true, same as Gen 3
    s.shot("12c_edit_info", "Gen-3 parity: A inside the VIEW/EDIT summary flips to edit "
                             "mode — no Item/Friendship rows (gbe_fields() drops them "
                             "outside GB_GEN2)")
    s.tap("B", settle=BIG_SETTLE)          # edit -> view
    s.tap("B", settle=BIG_SETTLE)          # back to box grid (no edits made -> no confirm)

    s.tap("A", settle=BIG_SETTLE)          # mon menu
    s.press_n("DOWN", 2)                   # VIEW/EDIT -> LEGALITY -> MOVE TO BOX
                                            # (UX-parity audit, 2026-09-07: LEGALITY now sits
                                            # right after VIEW/EDIT, matching Gen 3's own
                                            # occupied-mon-menu order -- was DOWN x1)
    s.tap("A", settle=BIG_SETTLE)
    # Party is the LAST row in the box/party picker for a Gen-1 source (gb_edit.c
    # GEN1_PARTY_BOX has the highest box number, so it sorts last). The picker's cursor
    # wraps (sel == 0 ? n-1 : sel-1 on UP, matching every other list in this app), so one
    # UP from the default top-of-list selection always lands on the last entry -- no
    # need to guess how many storage boxes this save has.
    s.tap("UP")
    s.shot("12d_move_to_party_selected", "S2b: MOVE TO BOX picker, Party selected")
    s.tap("A", settle=BIG_SETTLE)
    # BACKLOG #251 shots-refresh re-derivation (was: "gb_session.c gbs_move() checks
    # capacity BEFORE the Gen-1-specific check, so this shows MOVE REFUSED / The party
    # is full."): reproduced live and that is no longer what this tap sequence reaches.
    # Picking "Party" off the MOVE TO BOX list now hits pdna_gen12.c's own PDNA_DELTA
    # write-gate (gb_persist()'s "Edits are in-session only in the emulator build."
    # msg_wait -- the SAME generic refusal every other GB commit path in this file
    # shows) BEFORE gbs_move()'s capacity check ever runs at all -- confirmed by hand:
    # pressing A again on this dialog dismisses it straight back to the box grid, no
    # "party is full" screen in between. Likely BACKLOG #246 moving the write-gate
    # earlier in the MOVE TO flow (this lane did not chase the exact commit -- the
    # pixels are the evidence, not a theory). The capacity-check screen this shot used
    # to prove is therefore UNREACHABLE from this emulator vehicle now, same class as
    # 12f's own long-standing skip below -- shooting what is actually on screen
    # instead of the stale claim.
    s.shot("12e_gen1_party_full", "S2b: Gen 1 MOVE TO -> Party: the emulator's own "
                                   "PDNA_DELTA write-gate (\"Edits are in-session only "
                                   "in the emulator build.\") now fires BEFORE gbs_move()'s "
                                   "capacity check ever runs — the \"party is full\" "
                                   "screen (BACKLOG #40(d)'s wording fix) this shot used "
                                   "to show is no longer reachable from here; A dismisses "
                                   "straight back to the box grid with no move attempted")
    s.skip("12f_gen1_needs_base_text",
           "the \"Gen 1: withdraw it in-game instead\" text (GBS_ERR_NEEDS_BASE) is only "
           "reached when the destination party has a free slot AND gbs_move()'s own "
           "capacity check is reached at all -- both of Guy's Gen-1 saves (Red.sav, "
           "Yellow.sav) have a full 6/6 party, AND (BACKLOG #251 re-derivation) the "
           "emulator's own PDNA_DELTA write-gate now fires first regardless (see "
           "12e_gen1_party_full's own comment above). Needs a save with room in the "
           "party AND real hardware (no write-gate) to ever show this exact string.")

    s.skip("11_dv_orphan_warning", "needs a sidecar file on SD (docs/GEN3-TO-GB-SIDECAR-DESIGN.md "
                                    "section 10) — the emulator has no SD card, so has_sidecar is "
                                    "always false and the warning never fires. Real-hardware only.")
    return s


def run_red_create(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """BACKLOG #50, re-shot 2026-09-07/08 for the UX-parity rewrite — the SAME flow
    run_gold_create() drives, on the generation that has no Gender row (BACKLOG #51 is
    Gen-2-only, gbe_has_gender_row refuses outright for GB_GEN1) and whose pick_species()
    ceiling is 151, not 251 (pick_species_set_max_dex(gb_max_species(GB_GEN1))). "GB
    BOX5" (R press count 4 from the box grid's own default entry) is 19/20 on this
    corpus — one real empty slot. A Gen-1 ceiling excludes TWO filters (Gen 2 AND
    Gen 3+) instead of Gen-2's one (Gen 3+ only) -- both are skipped together on
    whichever R press first reaches them, so "All" -> "Gen 1" -> "Legendary" takes
    only 2 presses here, not the unrestricted picker's 4."""
    s = Session(core_mod, image_mod, rom, out_dir, "red_")
    print("== Red.sav (Gen 1) -- BACKLOG #50 CREATE (UX-parity re-shoot) ==")
    s.tap("A", settle=BIG_SETTLE)          # info -> box grid
    # BIG_SETTLE, not SETTLE (BACKLOG #278): the box-switch repaint outlasts SETTLE's 12 frames,
    # so the next R lands mid-repaint and is swallowed -- 4 fast presses stopped on BOX4 (20/20,
    # NO empty slot), and CREATE's tap then opened the Lapras summary. 4 presses with a full
    # settle each reach GB BOX5 (19/20).
    s.press_n("R", 4, settle=BIG_SETTLE)   # -> GB BOX5, 19/20 (one real empty slot)
    s.press_n("DOWN", 3)
    s.press_n("RIGHT", 1)                  # cursor -> slot 19, the one real empty slot
    s.shot("13_cursor_on_empty", "#50: Gen 1 — cursor on the one real empty slot (GB BOX5, 19/20)")

    s.tap("A", settle=BIG_SETTLE)
    # Red.sav's own fused image carries NO clip (run_red()'s own docstring: "no clip"),
    # so PASTE HERE correctly never appears here regardless of BLOCKING-1's fix --
    # this shot alone does not demonstrate the fix (the SAME menu construction code
    # runs either way; only the clip's own presence differs). See gold_14b_empty_menu
    # for the fix shown BOTH ways (with a real clip present).
    s.shot("13b_empty_menu", "#50: Gen 1's empty-cell menu -- CREATE, CANCEL (no PASTE HERE: "
                              "this run's own fused image carries no clip -- see gold_14b_empty_menu "
                              "for BLOCKING-1's fix shown with one present); row list otherwise "
                              "identical to Gen 2's (gb_create_hook does not special-case the "
                              "generation for the menu itself, only for the ROM lookup and the "
                              "moveset merge behind it)")

    s.tap("A", settle=BIG_SETTLE)          # select CREATE -> pick_species(1), restricted to 151
    s.shot("13c_species_picker", "#50: UX-parity -- Gen 1's CREATE also opens pick_species() -- "
                                  "restricted to dex 1..151 (pick_species_set_max_dex, "
                                  "gb_max_species(GB_GEN1)), Bulbasaur pre-selected")

    # UX-parity restriction demo: filter id 1 ("Gen 1") is never excluded by
    # filter_usable() regardless of the ceiling, so the first R still stops there
    # normally (All -> Gen 1) -- it is the SECOND R that then jumps straight over
    # BOTH "Gen 2" (152..251) and "Gen 3" (252+), both empty under a 151 ceiling,
    # landing on Legendary in one keypress. (An earlier draft of this script/caption
    # claimed ONE press did this -- caught by actually looking at the screenshot,
    # which still read "[Gen 1]", not "[Legendary]", after only one R.)
    s.press_n("R", 2, settle=SETTLE)
    s.shot("13c2_species_filter_skips_gen2_and_3", "#50: UX-parity -- 2x R cycles All -> Gen 1 -> "
                                                    "Legendary -- the SECOND press jumps straight "
                                                    "over both \"Gen 2\" and \"Gen 3\" in one "
                                                    "keypress, both empty under this Gen-1 ceiling "
                                                    "(filter_usable(), pdna_pick.c)")
    s.tap("L", settle=SETTLE)              # Legendary -> (skip Gen 3, Gen 2) -> Gen 1: one L back
    s.shot("13c3_species_filter_back_to_gen1", "#50: UX-parity -- one L press back to the \"Gen 1\" "
                                                "filter -- Bulbasaur (dex 1) selected again")

    s.tap("A", settle=BIG_SETTLE)          # pick Bulbasaur -> gb_create_learn (level + moveset)
    # BACKLOG #278: this shot used to assert the "Needs your Gen 1/2 ROM" wall. It is gone: the
    # generated Gen-1 base table (source/gb1_base_gen.c, #276) lets a no-ROM create build the
    # level-1 mon itself -- no choice screen, straight into the new mon's summary (CREATE mode).
    s.shot("13d_new_mon_summary", "#278: Gen 1 with no registered ROM -- picking Bulbasaur shows no "
                                   "choice prompt and builds the level-1 mon from the generated base "
                                   "table (source/gb1_base_gen.c): the new mon's summary opens in EDIT, "
                                   "INFO card, #001 BULBASAUR Lv1, Name BULBASAUR, GRASS/POISON chips",
           claim=["BULBASAUR", "Lv1", "EDIT", "INFO"],
           claim_absent=["Needs your Gen 1/2 ROM"])
    return s


def run_e4_settings(core_mod, image_mod, rom: Path, out_dir: Path) -> Session:
    """E4 (docs/SPRITE-ERA-DESIGN.md): Settings > Game ROM > the Sprites era grid.
    `rom` is pokedna-delta.gba fused with an ordinary GEN-3 .sav (tools/fuse_sav.py) --
    NOT a Game Boy image; this is the one path in this script that opens a normal
    Gen-3 box screen and drives the nav menu, not the GB fork the rest of the file is
    about. Reachable only as of commit b5788db ("Sprites grid reachable in the
    emulator build"): before that, PDNA_DELTA's Settings > Game ROM row was a flat
    "NO SD HERE" refusal with no live sub-screen at all.

    Navigation (session boots straight to the box screen; no info page here, unlike
    the GB fork's pdna_gen12_show_image): START -> nav menu (two 11-row columns,
    RIGHT jumps a whole column, sel 0 Party -> RIGHT -> sel 11 Events) -> DOWN x8 to
    NV_SETTINGS (source/pdna_layout.h's PDNA_NAV_ITEMS X-macro, index 19: column 1
    row 19-11=8 under PDNA_NAV_ROWS=11 -- BACKLOG #251 shots-refresh re-derivation:
    NV_MAP/NV_GB/NV_XFER were inserted ahead of NV_SETTINGS since this was last
    calibrated at DOWN x7 (then index 17 under PDNA_NAV_ROWS=10), which now lands on
    NV_XFER ("Transfers") instead -- reproduced live, the stale count opened the
    "TRANSFER RECORDS" screen, not Settings) -> A -> the Settings list
    (S_BACKUP..S_CLOSE) -> DOWN x3 to S_ROM -> A -> (PDNA_DELTA only) straight into
    sprite_settings(), no "Change ROM" popup in the way.
    """
    s = Session(core_mod, image_mod, rom, out_dir, "e4_")
    print("== E4: Settings > Sprites grid (fused Gen-3 save, no GB fork) ==")

    s.tap("START", settle=BIG_SETTLE)      # box screen -> nav menu
    s.tap("RIGHT")                          # column 0 (Party) -> column 1 (Events)
    s.press_n("DOWN", 8)                    # Events -> ... -> Settings (index 19, row 8)
    s.tap("A", settle=BIG_SETTLE)           # nav menu -> Settings list
    s.press_n("DOWN", 3)                    # Backups -> Animations -> Yard -> Game ROM
    s.tap("A", settle=BIG_SETTLE)           # S_ROM -> (delta) straight into the grid

    s.shot("01_sprites_default", "E4: Settings > Game ROM > Sprites — default state, "
           "every cell NATIVE; the GEN1/GEN2 rows show \"-\" under PC (the two dead "
           "cells se_cell_applies() defines: a Game Boy kind has no Gen-3-style PC box)")

    # No ROM is registered in this fused-save session (there is no second file for the
    # emulator to have registered), so se_era_next() can only ever offer NATIVE back --
    # cycling the selected cell with A is a real keypress but a visible no-op. Per the
    # coordinator's own fallback: also move the cursor so the second shot still differs
    # from the first, and say so plainly rather than imply a cycle that did not happen.
    s.tap("A", settle=SETTLE)               # attempt to cycle RS/PC -> stays "Native"
    s.tap("RIGHT", settle=SETTLE)           # PC -> PTY
    s.press_n("DOWN", 2, settle=SETTLE)     # RS -> EM -> FRLG

    s.shot("02_sprites_cursor_moved",
           "E4: A on a cell with no ROM registered leaves it NATIVE (se_era_next has "
           "nothing else to offer) — cursor moved to FRLG/PTY so this shot still "
           "differs from the default; cycling to a real era needs a registered ROM, "
           "which the fused-save emulator session has no second file to provide "
           "(hardware-only, docs/HW-TEST-2026-09-05-GB-ARC.md §K)")

    s.tap("B", settle=BIG_SETTLE)           # save (cfg_save no-ops: no SD) -> Settings list
    s.tap("B", settle=BIG_SETTLE)           # Close/B -> back to nav menu or box grid
    return s


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--gold", type=Path, help="pokedna-delta.gba fused with Gold.sav (+ --clip)")
    ap.add_argument("--red", type=Path, help="pokedna-delta.gba fused with Red.sav")
    ap.add_argument("--e4-emerald", type=Path,
                     help="pokedna-delta.gba fused with an ordinary Gen-3 .sav "
                          "(tools/fuse_sav.py) -- E4: Settings > Sprites grid, NOT a "
                          "Game Boy image")
    ap.add_argument("--out", type=Path, default=ROOT / "docs" / "shots" / "gb")
    a = ap.parse_args(argv)

    runs = [("gold", a.gold, (run_gold, run_gold_paste, run_gold_gender, run_gold_create)),
            ("red", a.red, (run_red, run_red_create)),
            ("e4-emerald", a.e4_emerald, (run_e4_settings,))]
    active = [(label, p, fns) for label, p, fns in runs if p is not None]
    if not active:
        sys.exit("nothing to do: pass at least one of --gold/--red/--e4-emerald")
    for label, p, _fns in active:
        if not p.is_file():
            sys.exit(f"--{label}: {p}: not a file")
    a.out.mkdir(parents=True, exist_ok=True)

    core_mod, image_mod = load_mgba()

    ok, skipped = [], []
    any_claim_failed = False
    try:
        for _label, p, fns in active:
            for fn in fns:
                sess = fn(core_mod, image_mod, p, a.out)
                ok += sess.taken; skipped += sess.skipped
                any_claim_failed = any_claim_failed or sess.any_claim_failed
    finally:
        # BACKLOG #179 A3 review D1: flush every Session that attached --vsd, no matter
        # how the loop above exited (a no-op today -- this script takes no --vsd flag of
        # its own -- but a Session built with vsd_img= directly would otherwise still
        # lose its writes, same blocker dgb_shots.main() had).
        flush_live_vsd_sessions()

    # A manifest, not just a list printed to stdout: tools/gb_contact_sheet.py reads this
    # so a shot's caption lives in exactly one place (this file) instead of being
    # retyped wherever the sheet gets built.
    #
    # MERGED with whatever is already there, not overwritten: a run given only
    # --e4-emerald (say) must not erase the S1..S5-B entries a separate --gold/--red
    # run already wrote — this file is the union of every partial run against the
    # same --out, keyed by filename/name so re-running a given shot updates its own
    # caption in place instead of duplicating it.
    import json
    manifest_path = a.out / "manifest.json"
    existing = {"shots": [], "skipped": []}
    if manifest_path.is_file():
        existing = json.loads(manifest_path.read_text(encoding="utf-8"))
    by_file = {e["file"]: e for e in existing.get("shots", [])}
    for n, c, claim_info in ok:
        entry = {"file": n, "caption": c}
        entry.update(claim_info)
        by_file[n] = entry
    by_name = {e["name"]: e for e in existing.get("skipped", [])}
    for n, r in skipped:
        by_name[n] = {"name": n, "reason": r}
    manifest_path.write_text(
        json.dumps({"shots": list(by_file.values()), "skipped": list(by_name.values())}, indent=2),
        encoding="utf-8")

    print(f"\n{len(ok)} shot(s) saved to {a.out}")
    for name, reason in skipped:
        print(f"[skip] {name}: {reason}")
    # BACKLOG #184: a claim= failure is never silent -- the PNG/manifest entry above are
    # still written (the frame is evidence either way), but the RUN reports failure.
    if any_claim_failed:
        print("\n[CLAIM FAILED] one or more shots -- see [CLAIM FAILED] lines above "
              "and each entry's manifest.json \"claim_failed\" list", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
