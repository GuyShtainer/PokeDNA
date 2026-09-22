#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""gb_retail_gate.py — S4: put the retail-boot gate into the regression loop on
EDITED Gen-1/2 saves.

tools/gb_roundtrip.py already proves a save against the real ROM, but every save it has
ever booted was either untouched or hand-corrupted for its own selftest. This script is
the missing middle: it builds saves that a REAL EDITOR would produce — a rewritten
nickname, a changed level, a deleted party member, a Pokemon moved box<->party — using
this tree's own editing pipeline (gb_session + gb_edit + gb_editor, via the CLI in
tests/host_gbsurgery_tool.c, "the surgery tool" below), boots each one in mGBA, and
asserts on what the GAME shows, not on what our own parser thinks it wrote.

Run it with: /usr/local/bin/python3 tools/gb_retail_gate.py
(needs the mGBA python bindings — pass --mgba-vendor if they are not already importable;
the default below points at this workspace's vendored copy). `make retail-gate` runs it
with the right interpreter and vendor path already filled in.

THREE GATES run on every edited boot, and NONE of them is redundant --
docs/GEN12-EDIT-DESIGN.md trap 7 exists precisely because a save can boot, show the
right trainer, and still have quietly lost the edit:

  CONTENT — an assertion read off the screen the game itself drew: gb_roundtrip.py's
    --expect-party/--expect-no-screen/--expect-party-count, PLUS (nickname/level cases)
    a SLOT-ANCHORED re-read of the ordered rows in rep["party_screen"] (see
    check_nick_slot/check_level_slot) -- gb_roundtrip's own --expect-party is a substring
    search over the WHOLE party blob, so it cannot tell "my edit landed in slot 0" from
    "my edit landed in slot 1 instead", which is a real bug this gate caught.

  SRAM DIFF (sram_changed_primary_bytes / sram_changed_bytes) — gb_roundtrip.py computes
    this for every run; secondary_gate() reads it back. Its role differs by generation,
    and the difference is NOT cosmetic:
      Gen 2 -- THIS is the ONLY thing that can detect a stale-primary silent discard in
        THIS tree, and CONTENT CANNOT SUBSTITUTE FOR IT. gen2_write.c / gb_session.c
        write every edit into BOTH the primary and the backup copy at commit time, so an
        edited save's backup already carries the edit before it ever reaches mGBA --
        unlike the original Crystal.sav corpus file, whose backup is stale from years of
        untouched retail play. Proven: the reviewer corrupted just the primary's
        checksum on an already-edited Crystal save; the game booted the (already-edited)
        backup, verdict=accept, the edited nickname right there on screen -- CONTENT
        assertions saw nothing wrong. Only sram_changed_primary_bytes (1 byte, nonzero)
        caught it. NEVER weaken or drop this check for Gen 2.
      Gen 1 -- no primary/backup split exists at all (gb_roundtrip.py's _classify()
        never sets a `lay` for gen 1, so everything non-scratch lands in "other"), and
        gb_roundtrip's own selftest asserts a good Gen-1 load rewrites NO save byte.
        sram_changed_bytes (scratch already excluded by gb_roundtrip) is the signal here.
    Either way this is not a bound on "how big was our edit": the diff is BEFORE-LOAD vs
    AFTER-LOAD, and our edit already happened offline before the emulator ever ran, so a
    byte-count threshold on it would be measuring the wrong thing entirely.

  LENGTH GUARD — edited_case() stat()s the file it is about to boot against the
    ORIGINAL save's length BEFORE booting at all (trap 8), and separately, after booting,
    diff_dump() re-derives the SRAM/RTC-tail split (split_rtc_tail, reused from
    gb_roundtrip.py, never reimplemented) between the dump and the edited input. The
    reported byte count there is the WHOLE SRAM span INCLUDING scratch (Gen 1 sprite
    buffers, Gen 2 decompression scratch + window stack) and is informational only --
    never asserted on, since it churns by design and its size depends only on where the
    boot happened to stop. Only a LENGTH change there is a hard failure.

Skips are explicit: a case whose precondition the corpus doesn't meet (a party under 2
members, no box with a free slot) prints "[skip] <save> <case> -- <reason>" and is never
folded into a silent pass.

What this gate does NOT run through run_host_tests.py's default loop: a full pass is
~20-30 emulator boots and several seconds each. See that script's own docstring.
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent            # tools/
PROJECT_ROOT = HERE.parent                          # PokeDNA/
ROUNDTRIP = HERE / "gb_roundtrip.py"

sys.path.insert(0, str(HERE))
import gb_roundtrip as grt   # noqa: E402 — needs sys.path set first; split_rtc_tail only, no mgba import at module load

DEFAULT_CORPUS = Path("/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb")
DEFAULT_SCRATCH = Path("/tmp/gb_retail_gate")
DEFAULT_VENDOR = Path("/Users/guyshtainer/VSCodeProjects/gba-toolkit/projects/rec2mp4/vendor")

SURGERY_SRCS = [
    "tests/host_gbsurgery_tool.c", "source/gb_session.c", "source/gb_editor.c",
    "source/gb_edit.c", "source/gen1_save.c", "source/gen1_write.c",
    "source/gen2_save.c", "source/gen2_write.c", "source/data_tables.c",
    # BACKLOG #50: --op create's own dependencies (rom_gbsprite/rom_gbbase/rom_gblearn
    # locate the ROM's tables; gb_new_mon builds the record from what they find).
    "source/rom_gbsprite.c", "source/gb_sprite_codec.c", "source/rom_gbbase.c",
    "source/rom_gblearn.c", "source/gb_new_mon.c",
    "source/gb_trainer.c", "source/gb_fields.c",
    # BACKLOG #49 P2a: --op item's own dependency (gb_bag.h's pure-C bag/PC-item core).
    "source/gb_bag.c",
    # BACKLOG #85/#86/#90/#94: --op daycare/clock/fly/boxname's own dependencies.
    "source/gb_daycare.c", "source/gb_clock.c", "source/gb_fly.c", "source/gb_boxnames.c",
    # BACKLOG #88: --op flagset/counter's own dependencies.
    "source/gb_flags.c", "source/gb_flags_rw.c",
    # BACKLOG #89: --op hofclear/hofcount's own dependency.
    "source/gb_hof.c",
    # BACKLOG #87 item 6: --op dexset's own dependency (gb_dex.h's owned/seen core).
    "source/gb_dex.c",
]

# gen: 1 = Gen-1 numbering (no primary/backup mirror split in gb_roundtrip's classifier,
# Gen-1 box->party is refused by gb_session by design — needs base stats this tree does
# not carry); 2 = Gen-2 (Gold/Crystal both go through the same G/S-vs-Crystal mirror map).
GAMES = {
    "red":     {"rom": "Red.gb",      "sav": "Red.sav",      "gen": 1},
    "yellow":  {"rom": "Yellow.gb",   "sav": "Yellow.sav",   "gen": 1},
    "gold":    {"rom": "Gold.gbc",    "sav": "Gold.sav",     "gen": 2},
    "crystal": {"rom": "Crystal.gbc", "sav": "Crystal.sav",  "gen": 2},
}

NEW_NICK = "GATEX"          # pure-letter ASCII, well inside GB_NICK_GLYPHS (10) — every
                             # Game Boy generation's charset can show it as typed
BOX_CAPACITY = 20           # both generations' storage boxes (gb_edit.c gen1/g2_list_capacity)

MIRROR_NICK = "BAKMIRR"     # distinct from NEW_NICK so a log line is unambiguous about
                             # which case produced it (BACKLOG #49 P0's mirror-rescue case)
# LE16 file offset of the STORED PRIMARY checksum, source/gen2_save.c's k_stored_off —
# the byte this case corrupts on purpose. Not the backup offset: this case's whole point
# is proving the BACKUP survives when the PRIMARY is the one that gets damaged.
PRIMARY_CKSUM_OFF = {"gs": 0x2D69, "crystal": 0x2D0D}

# BACKLOG #49 P0's field-write primitive (gbs_write_field/gbs_finish), proven with money:
# the one field docs/GEN12-PARITY-DESIGN.md §4.2 tabulates a post-load WRAM anchor for on
# every one of the four games. RAM addresses from that table (all VERIFIED the first time
# this case boots and reads the value it wrote, per the table's own footnote).
MONEY_VALUE = 123456        # < the 999999 cap; distinct digits so a byte-order bug shows
MONEY_WRAM = {"red": 0xD347, "yellow": 0xD346, "gold": 0xD573, "crystal": 0xD84E}
MONEY_LEN = 3                # both encodings are 3 bytes (BCD24 Gen 1 / BE24 Gen 2)

# BACKLOG #95 review gate case: the held-item write (--op helditem, gb_set_held_item)
# proven the SAME way as money above -- boot it and read the byte back off WRAM,
# because a held item is not readable off any screen this driver scrapes either.
# Gold and Crystal only (review's own ask): party slot 0's record starts at wPartyMon1
# and record offset 0x01 is R2_ITEM (source/gb_edit.c, R2_ITEM = 0x01) --
#   Gold:    wPartyMon1 = bank 01, $DA2A (assets/upstream/pokegold/symbols/pokegold.sym)
#   Crystal: wPartyMon1 = bank 01, $DCDF (assets/upstream/pokecrystal/symbols/pokecrystal.sym)
# so the held-item byte is $DA2B / $DCE0. Both addresses are in the $D000-$DFFF banked
# region, same as MONEY_WRAM's own gold/crystal entries -- read_mem's svbk_ok gate
# (already exercised by run_money_case) covers this the same way.
HELDITEM_VALUE = 0x05        # Potion -- distinct from 0 (None) and any real party item
HELDITEM_WRAM = {"gold": 0xDA2B, "crystal": 0xDCE0}

# gbmon lane, BACKLOG #95 review C11's own binding test: --op caught (source/gen3_to_gb.c
# / source/pdna_gen12.c's gb_mark_caught / source/gb_session.c's gb_session_is_crystal)
# writes wPartyMon1CaughtLevel -- (time_of_day<<6)|level packed into ONE byte, verified
# against pokecrystal/engine/pokemon/caught_data.asm's SetBoxmonOrEggmonCaughtData
# (`ld a, [wTimeOfDay] / rrca / rrca ... or [wCurPartyLevel] ... ld [hli], a`). Symbol
# verified against assets/upstream/pokecrystal/symbols/pokecrystal.sym:
#   01:dcfc wPartyMon1CaughtLevel
# Crystal only -- Gold/Silver has no such symbol (those two bytes are Unused1/Unused2,
# gen2_save.c:493-497) and the surgery step itself refuses before there is anything to
# boot, so CAUGHT_WRAM has no "gold" entry (mirrors HELDITEM_WRAM's Gen-1 absence).
CAUGHT_TIME, CAUGHT_LEVEL, CAUGHT_LOC, CAUGHT_GENDER = 1, 40, 5, 0   # 1 = morning
CAUGHT_WANT_BYTE = (CAUGHT_TIME << 6) | CAUGHT_LEVEL                 # 0x68
CAUGHT_WRAM = {"crystal": 0xDCFC}

# BACKLOG #49 P1a — gb_trainer.h's badges/name setters (source/gb_trainer.c), via the
# surgery tool's new --op badges / --op name. WRAM anchors from
# docs/GEN12-PARITY-DESIGN.md §4.2's own table, derived from the pinned `symbols`
# branches (pret/pokered wPlayerName=D158/wObtainedBadges=D356; pret/pokeyellow same
# fields -1 byte; pret/pokegold wPlayerName=D1A3, wJohtoBadges=D57C, wKantoBadges=D57D
# contiguous; pret/pokecrystal wPlayerName=D47D, wJohtoBadges=D857, wKantoBadges=D858
# contiguous) — each becomes VERIFIED the first time this case boots and reads it back.
TRAINER_NAME = "GATET"       # <=7 glyphs; distinct from NEW_NICK/MIRROR_NICK so a log
                             # line is unambiguous about which case produced it
# P1a review D8: writing the SAME mask to both Gen-2 badge bytes makes a Johto/Kanto
# swap invisible to this gate (want_badges would read the same either way). Two
# distinct masks close that hole; Gen 1's single BADGES byte still just gets
# BADGES_MASK (there is only one byte to check there).
BADGES_MASK = 0x0F           # Gen 1: BADGES; Gen 2: Johto
BADGES_MASK_KANTO = 0xF0     # Gen 2: Kanto -- distinct nibble from BADGES_MASK, so a
                             # swap reads "f00f" instead of the expected "0ff0"
NAME_WRAM = {"red": 0xD158, "yellow": 0xD157, "gold": 0xD1A3, "crystal": 0xD47D}
NAME_LEN = 11
BADGES_WRAM = {"red": 0xD356, "yellow": 0xD355, "gold": 0xD57C, "crystal": 0xD857}

# BACKLOG #49 P2a -- gb_bag.h's pure-C bag/PC-item core (source/gb_bag.c), via the
# surgery tool's new `--op item POCKET ID QTY`. WRAM anchors DERIVED HERE, not copied
# from the design doc's own table (which only lists the COUNT address for Gen 2's
# Key items/Balls/PC pockets) -- each is `grep -iE 'wNumBagItems|wBagItems|wNumItems|
# wItems|wNumBalls|wBalls' assets/upstream/<repo>/symbols/<repo>.sym`:
#   pokered.sym       00:d31d wNumBagItems   00:d31e wBagItems   (body = count + 1)
#   pokeyellow.sym     00:d31c wNumBagItems   00:d31d wBagItems
#   pokegold.sym       01:d5b7 wNumItems      01:d5b8 wItems      01:d5fc wNumBalls  01:d5fd wBalls
#   pokecrystal.sym    01:d892 wNumItems      01:d893 wItems      01:d8d7 wNumBalls  01:d8d8 wBalls
# Every count/body pair is contiguous (body starts the byte right after its own count),
# matching the FILE layout's own "count byte, then body" shape (§1.2) -- WRAM is simply
# where the live session keeps that same struct while the game runs.
ITEMS_WRAM = {"red": 0xD31D, "yellow": 0xD31C, "gold": 0xD5B7, "crystal": 0xD892}
BALLS_WRAM = {"gold": 0xD5FC, "crystal": 0xD8D7}   # Gen 2 only

# The FILE-offset side of the same pockets (Appendix B / this design's own field
# table, source/gb_fields.c GBF_BAG_COUNT/BODY and GBF_BALLS_COUNT/BODY) -- used to
# inspect the ORIGINAL save's own bag contents before editing, so this case can tell
# whether inserting ITEM_ID will APPEND a new entry or MERGE into one already there
# (docs/GEN12-PARITY-DESIGN.md §1.2's own merge rule, gb_bag.h's gbb_insert) and
# compute the right WRAM address either way, rather than assuming the corpus save
# happens to be entry-free of the id this case plants.
BAG_FILE_OFF = {
    "red":     {"count": 0x25C9, "body": 0x25CA, "cap": 20},
    "yellow":  {"count": 0x25C9, "body": 0x25CA, "cap": 20},
    "gold":    {"count": 0x241F, "body": 0x2420, "cap": 20},
    "crystal": {"count": 0x2420, "body": 0x2421, "cap": 20},
}
BALLS_FILE_OFF = {   # Gen 2 only
    "gold":    {"count": 0x2464, "body": 0x2465, "cap": 12},
    "crystal": {"count": 0x2465, "body": 0x2466, "cap": 12},
}

# POTION (a real item, both generations -- assets/upstream/*/constants/item_constants.asm:
# pokered POTION = $14; pokegold/pokecrystal POTION = $12, item ids were renumbered for
# Gen 2). MASTER_BALL ($01, both pokegold/pokecrystal item_constants.asm) for the Balls
# pocket -- the design's own P2a item asks for "a Potion x7 ... and one Gen-2 Ball".
POTION_ID = {"red": 0x14, "yellow": 0x14, "gold": 0x12, "crystal": 0x12}
BALL_ID = 0x01
POTION_QTY = 7
BALL_QTY = 3


def _find_or_append(orig: bytes, count_off: int, body_off: int, cap: int, item_id: int):
    """Read the pocket's CURRENT count + (id,qty) entries straight out of the ORIGINAL
    save bytes (entry size 2: id, qty -- every pocket this case touches has a
    quantity). Returns (entry_index, new_count): `item_id` already present -> that
    slot's own index and `new_count == count` (a SET never grows the pocket, do_item's
    own insert-or-set contract); not present -> a fresh append at `entry_index ==
    count` and `new_count == count + 1`. The resulting quantity is always exactly
    what the caller passes to `--op item` (SET, never merged/added)."""
    count = orig[count_off]
    n = min(count, cap)
    for i in range(n):
        eid = orig[body_off + i * 2]
        if eid == 0xFF:
            break
        if eid == item_id:
            return i, count
    return n, count + 1


def run_bag_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #49 P2a -- gb_bag.h's gbb_read/gbb_insert/gbb_set_qty/gbb_write (source/
    gb_bag.c), via the surgery tool's new `--op item POCKET ID QTY` (insert-or-set: SET
    an id already present, else insert a brand-new entry -- see do_item's own comment).
    Plants a Potion x7 in the Items pocket on every game, plus one Master Ball in the
    Balls pocket on the two Gen-2 games (Gen 1 has no separate Balls pocket -- Poke
    Balls are ordinary bag items there, §1.2).

    Like run_money_case/run_trainer_case, this is not screen-scraped (no bag-pocket
    screen this driver's content reader reaches) -- it is asserted purely off WRAM,
    gated on SVBK the same way (§4.2 rule 1), and the exact byte address is computed
    from the ORIGINAL save's own current bag contents (_find_or_append) so a corpus
    save that already happens to carry a Potion is a MERGE case (SET at its existing
    slot) rather than a silently-wrong assumption that every corpus save is Potion-free."""
    label = "bag item (Potion, BACKLOG #49 P2a)"
    item_id = POTION_ID[name]
    off = BAG_FILE_OFF[name]
    idx, new_count = _find_or_append(sav.read_bytes(), off["count"], off["body"],
                                     off["cap"], item_id)
    ops = [["item", "items", str(item_id), str(POTION_QTY)]]

    ball_idx = ball_new_count = None
    if info["gen"] == 2:
        boff = BALLS_FILE_OFF[name]
        ball_idx, ball_new_count = _find_or_append(sav.read_bytes(), boff["count"],
                                                    boff["body"], boff["cap"], BALL_ID)
        ops.append(["item", "balls", str(BALL_ID), str(BALL_QTY)])

    edited = work / "bag.sav"
    rc, out, err = run_surgery(binary, sav, edited, ops)
    if rc != 0:
        tally.record(label, False, f"surgery refused: {err.strip()}")
        return

    items_count_addr = ITEMS_WRAM[name]
    items_entry_addr = items_count_addr + 1 + idx * 2
    read_args = ["--read-mem", f"{items_count_addr:#06x}:1",
                "--read-mem", f"{items_entry_addr:#06x}:2"]
    if info["gen"] == 2:
        balls_count_addr = BALLS_WRAM[name]
        balls_entry_addr = balls_count_addr + 1 + ball_idx * 2
        read_args += ["--read-mem", f"{balls_count_addr:#06x}:1",
                     "--read-mem", f"{balls_entry_addr:#06x}:2"]

    rc, rep, out, err = boot(python, rom, edited, work / "bag", vendor, work / "bag.json",
                             extra_args=["--expect", "accept"] + read_args)
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))

    want_items_count = f"{new_count:02x}"
    want_items_entry = f"{item_id:02x}{POTION_QTY:02x}"
    got_items_count = mem.get(f"{items_count_addr:#06x}")
    got_items_entry = mem.get(f"{items_entry_addr:#06x}")
    ok = (rc == 0 and svbk_ok and got_items_count == want_items_count and
         got_items_entry == want_items_entry)
    detail = (f"verdict={rep.get('verdict')} svbk={mem.get('svbk')} svbk_ok={svbk_ok} "
             f"items count@{items_count_addr:#06x}={got_items_count!r} want={want_items_count!r} "
             f"items entry@{items_entry_addr:#06x}={got_items_entry!r} want={want_items_entry!r}")

    if info["gen"] == 2:
        want_balls_count = f"{ball_new_count:02x}"
        want_balls_entry = f"{BALL_ID:02x}{BALL_QTY:02x}"
        got_balls_count = mem.get(f"{balls_count_addr:#06x}")
        got_balls_entry = mem.get(f"{balls_entry_addr:#06x}")
        ok = (ok and got_balls_count == want_balls_count and
             got_balls_entry == want_balls_entry)
        detail += (f" | balls count@{balls_count_addr:#06x}={got_balls_count!r} "
                  f"want={want_balls_count!r} balls entry@{balls_entry_addr:#06x}="
                  f"{got_balls_entry!r} want={want_balls_entry!r}")

    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record(label, ok, detail)


# U5 (BACKLOG #67, Gen-2's own Pack): a TM/HM count-array case -- Gen 2 only (Gen 1
# has no TM/HM pocket at all, gb_bag.h's own table). `wTMsHMs`, grep -iE 'wTMsHMs'
# assets/upstream/<repo>/symbols/<repo>.sym: pokegold.sym 01:d57e, pokecrystal.sym
# 01:d859 (a flat 57-byte array, index = TM/HM number - 1, no count/body split unlike
# every other pocket -- gb_bag.h's own GBB_POCKET_TMHM comment). File offset from
# source/gb_fields.c's own GBF_TMHM_COUNTS row (gold 0x23e6, crystal 0x23e7, 57 B).
TMHM_WRAM = {"gold": 0xD57E, "crystal": 0xD859}
TMHM_FILE_OFF = {"gold": 0x23E6, "crystal": 0x23E7}
TMHM_INDEX = 0     # TM01
TMHM_COUNT = 42


def run_tmhm_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """gb_bag.h's gbb_tmhm_get/gbb_tmhm_set (source/gb_bag.c), via the surgery tool's
    `--op item tmhm INDEX COUNT` (do_item's own TMHM branch, tests/host_gbsurgery_
    tool.c) -- sets TM01's own count to 42 and asserts the exact WRAM byte, the same
    WRAM-not-screen-scraped posture run_bag_case above uses (no TM/HM-pocket screen
    this driver's content reader reaches either)."""
    label = "TM/HM count (TM01, U5, BACKLOG #67)"
    ops = [["item", "tmhm", str(TMHM_INDEX), str(TMHM_COUNT)]]
    edited = work / "tmhm.sav"
    rc, out, err = run_surgery(binary, sav, edited, ops)
    if rc != 0:
        tally.record(label, False, f"surgery refused: {err.strip()}")
        return

    tmhm_addr = TMHM_WRAM[name] + TMHM_INDEX
    read_args = ["--read-mem", f"{tmhm_addr:#06x}:1"]
    rc, rep, out, err = boot(python, rom, edited, work / "tmhm", vendor, work / "tmhm.json",
                             extra_args=["--expect", "accept"] + read_args)
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))

    want = f"{TMHM_COUNT:02x}"
    got = mem.get(f"{tmhm_addr:#06x}")
    ok = (rc == 0 and svbk_ok and got == want)
    detail = (f"verdict={rep.get('verdict')} svbk={mem.get('svbk')} svbk_ok={svbk_ok} "
             f"TM01 count@{tmhm_addr:#06x}={got!r} want={want!r}")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record(label, ok, detail)


def _bcd24_hex(v):
    """v (0..999999) as 3 BCD bytes, hex-encoded -- which is just its decimal digits,
    since a BCD nibble IS a decimal digit. Matches tests/host_gbsurgery_tool.c's
    encode_bcd24 exactly (re-derived here rather than shared, so the two independently
    agreeing is itself evidence)."""
    return f"{v:06d}"


def _be24_hex(v):
    """v as 3 big-endian binary bytes, hex-encoded. Matches encode_be24 in the surgery
    tool the same way."""
    return f"{v:06x}"

SCRATCH_MARKER = ".gb_retail_gate"   # written into every scratch dir this script creates


# --------------------------------------------------------------------------
# scratch-directory safety (a user-supplied --scratch is rmtree'd; guard it)
# --------------------------------------------------------------------------
def _contains(parent: Path, child: Path) -> bool:
    """True if `child` is `parent` itself or lives anywhere under it."""
    parent, child = parent.resolve(), child.resolve()
    return parent == child or parent in child.parents


def guard_scratch_path(scratch: Path, corpus: Path):
    """Refuse outright if --scratch could ever put the corpus at risk: rmtree(scratch)
    must never be able to reach `corpus` (or the reverse -- writing/removing scratch
    contents inside a directory this tool treats as READ ONLY is its own footgun)."""
    if _contains(scratch, corpus) or _contains(corpus, scratch):
        print(f"refusing --scratch {scratch.resolve()}: it equals, contains, or is "
              f"contained by the corpus directory {corpus.resolve()} -- this tool "
              "rmtree's its scratch dir and must never be able to touch the corpus",
              file=sys.stderr)
        sys.exit(2)


def prepare_scratch(scratch: Path):
    """rmtree an EXISTING scratch dir only if a previous run of THIS script created it
    (the marker file), so pointing --scratch at some unrelated directory by mistake
    (a typo, a shell-expansion accident) fails loudly instead of deleting it."""
    marker = scratch / SCRATCH_MARKER
    if scratch.exists():
        if not marker.exists():
            print(f"refusing to remove {scratch.resolve()}: it already exists and has "
                  f"no {SCRATCH_MARKER} marker, so it was not created by "
                  "gb_retail_gate.py. Point --scratch at an empty or gate-owned "
                  "directory.", file=sys.stderr)
            sys.exit(2)
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    marker.write_text("gb_retail_gate.py scratch directory -- safe to delete\n")


# --------------------------------------------------------------------------
# the surgery tool (tests/host_gbsurgery_tool.c)
# --------------------------------------------------------------------------
def build_tool(scratch: Path) -> Path:
    """cc -Wall -Wextra the surgery tool; ANY compiler output (not just a nonzero exit)
    is treated as a build failure, per D3 — this tool must compile silent-clean."""
    binary = scratch / "hgbsurg"
    cmd = (["cc", "-std=c11", "-Wall", "-Wextra", "-I", "source", "-I", "tests"]
           + SURGERY_SRCS + ["-o", str(binary)])
    proc = subprocess.run(cmd, cwd=PROJECT_ROOT, capture_output=True, text=True)
    noise = (proc.stdout + proc.stderr).strip()
    if proc.returncode != 0 or noise:
        print("BUILD FAILED (host_gbsurgery_tool.c must compile with zero warnings):",
              file=sys.stderr)
        print(noise, file=sys.stderr)
        sys.exit(2)
    return binary


def run_surgery(binary: Path, in_path: Path, out_path: Path, op_groups, rom: Path | None = None):
    """op_groups: list of token-lists, one per --op (e.g. ["nick","party","0","GATEX"]).
    `rom` (BACKLOG #50): the Gen-1/2 ROM --op create reads base stats/learnsets off --
    the SAME rom already booted (GAMES[name]["rom"] under the same corpus)."""
    cmd = [str(binary), "--in", str(in_path), "--out", str(out_path)]
    if rom is not None:
        cmd += ["--rom", str(rom)]
    for g in op_groups:
        cmd += ["--op"] + list(g)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    return proc.returncode, proc.stdout, proc.stderr


LIST_BOX_RE = re.compile(r"^box (\d+): count=(-?\d+)$")
LIST_PARTY_RE = re.compile(r"^party: count=(-?\d+)$")
LIST_SLOT_RE = re.compile(r"^  slot (\d+): dex=(\d+) level=(\d+) nick=(.*)$")


def parse_list(text: str):
    """--list's stdout -> {"box0": {"count": N, "slots": [...]}, ..., "party": {...}}."""
    sections, cur = {}, None
    for line in text.splitlines():
        m = LIST_BOX_RE.match(line)
        if m:
            cur = f"box{m.group(1)}"
            sections[cur] = {"count": int(m.group(2)), "slots": []}
            continue
        m = LIST_PARTY_RE.match(line)
        if m:
            cur = "party"
            sections[cur] = {"count": int(m.group(1)), "slots": []}
            continue
        m = LIST_SLOT_RE.match(line)
        if m and cur:
            sections[cur]["slots"].append(
                {"dex": int(m.group(2)), "level": int(m.group(3)), "nick": m.group(4)})
    return sections


def first_room_box(sections) -> int | None:
    """Lowest-index storage box with a free slot, or None if every box the --list saw is
    full. Corpus reality: box 0 is 20/20 on all four save files, so a literal "box 0" is
    never a legal party->box destination — this is why the destination is discovered, not
    hardcoded."""
    idx = 0
    while f"box{idx}" in sections:
        if sections[f"box{idx}"]["count"] < BOX_CAPACITY:
            return idx
        idx += 1
    return None


# --------------------------------------------------------------------------
# booting (subprocess over tools/gb_roundtrip.py)
# --------------------------------------------------------------------------
def boot(python, rom, sav, out_dir: Path, vendor, json_path: Path,
         dump_path: Path | None = None, extra_args=None):
    out_dir.mkdir(parents=True, exist_ok=True)
    cmd = [python, str(ROUNDTRIP), "--rom", str(rom), "--sav", str(sav),
           "--out", str(out_dir), "--mgba-vendor", str(vendor),
           "--json", str(json_path), "-q"]
    if dump_path is not None:
        cmd += ["--dump-save", str(dump_path)]
    cmd += list(extra_args or ())
    proc = subprocess.run(cmd, capture_output=True, text=True)
    rep = json.loads(json_path.read_text()) if json_path.exists() else {}
    return proc.returncode, rep, proc.stdout, proc.stderr


def stderr_tail(err: str, n: int = 5) -> str:
    """Last `n` non-blank lines of a subprocess's captured stderr, joined for a one-line
    detail string. Exists so a harness-level failure (mgba missing, a bad --mgba-vendor)
    shows up as "stderr: ModuleNotFoundError: No module named 'mgba'" instead of the
    caller silently printing "verdict=None" and leaving the real cause to a re-run with
    stderr un-captured."""
    lines = [l for l in err.strip().splitlines() if l.strip()]
    return " / ".join(lines[-n:])


def secondary_gate(rep: dict, gen: int):
    """The SRAM-diff gate (see module docstring): did the game's own load-time SRAM
    rewrite behave like an ACCEPTED save, independent of what the screen showed? For
    Gen 2 this is the ONLY detector of a stale-primary silent discard in this tree
    (gen2_write.c mirrors every edit into both save copies, so the CONTENT the game
    draws is identical whichever copy it actually booted) -- this branch must never be
    weakened or made conditional. For Gen 1 there is no primary/backup split at all; a
    good load rewrites no save byte, full stop."""
    if rep.get("harness_error"):
        return False, f"harness error: {rep['harness_error']}"
    if gen == 1:
        n = rep.get("sram_changed_bytes", 0)
        if n:
            return False, (f"a Gen-1 load rewrote {n} save byte(s) outside scratch — a "
                            "good load rewrites none")
        return True, "sram_changed=0 (as a good Gen-1 load must be)"
    pb = rep.get("sram_changed_primary_bytes", 0)
    if pb:
        return False, (f"the PRIMARY copy changed ({pb} byte(s)) — the game rejected it "
                        "and booted the backup")
    return True, (f"primary=0, backup={rep.get('sram_changed_backup_bytes', 0)} "
                  "(mirror sync only)")


PARTY_ROWS_PER_SLOT = 2   # every generation draws two screen rows per party member,
                          # measured off all four corpus saves' real party screens:
                          # Gen 1  row A "NICK...LEVEL[STATUS]"       row B "HP/MAXHP"
                          # Gen 2  row A "NICK...HP/MAXHP" (fused,     row B "LEVEL" alone
                          #        no separator once the name fills
                          #        its field)
                          # so slot i's own rows are always party_screen[2*i:2*i+2].


def party_rows_for_slot(party_screen, slot_idx):
    """The two consecutive rows this generation draws for party slot `slot_idx`
    (0-based), or None if the screen was never read or has fewer rows than that."""
    if not party_screen:
        return None
    i = PARTY_ROWS_PER_SLOT * slot_idx
    if i + 1 >= len(party_screen):
        return None
    return party_screen[i], party_screen[i + 1]


def slot_level(gen, row_a, row_b, nick):
    """The level digits for a slot whose nickname is `nick` (already confirmed to lead
    row_a by the caller). Gen 1 fuses the level onto the END of row_a, right after the
    (fixed-width) nickname field; Gen 2's row_a is name+HP with no level in it at all --
    the level is the ENTIRE contents of row_b. Levels ARE readable off the party screen
    (see docs/GEN12-EDIT-DESIGN.md S4 -- an earlier revision of this file claimed
    otherwise and was wrong)."""
    m = re.search(r"(\d+)", row_a[len(nick):] if gen == 1 else row_b)
    return int(m.group(1)) if m else None


def check_nick_slot(slot_idx, nick):
    """SLOT-ANCHORED nickname check. `--expect-party` alone is a substring search over
    the WHOLE party blob (gb_roundtrip.py evaluate()/party_count()), so a nickname
    written to the WRONG slot still passes it -- proven: rewriting the surgery op from
    slot 0 to slot 1 stayed green on Red and Crystal before this check existed. This
    reads the ORDERED rows out of the JSON report instead and requires THIS slot's own
    row to start with the new text."""
    def _check(rep):
        rows = party_rows_for_slot(rep.get("party_screen"), slot_idx)
        if rows is None:
            return False, f"no party row pair for slot {slot_idx}"
        return rows[0].startswith(nick), f"slot {slot_idx} row={rows[0]!r}"
    return _check


def check_level_slot(slot_idx, gen, nick, want_level):
    """SLOT-ANCHORED level check, same row-0 anchor as check_nick_slot(): confirms the
    UNCHANGED nickname (the level op never touches it) still leads this slot's row --
    catching the same wrong-slot class of bug -- AND reads the new level back off the
    row the game actually drew, instead of the vacuous fallback this file used to fall
    back to (asserting the ORIGINAL nickname was on screen at all: on Red that passed
    even with slot 0 renamed to ZZZ, because slot 1 is literally named MEWTWO and
    'MEW' is a substring of it)."""
    def _check(rep):
        rows = party_rows_for_slot(rep.get("party_screen"), slot_idx)
        if rows is None:
            return False, f"no party row pair for slot {slot_idx}"
        row_a, row_b = rows
        if not row_a.startswith(nick):
            return False, f"slot {slot_idx} row={row_a!r} does not start with {nick!r}"
        lvl = slot_level(gen, row_a, row_b, nick)
        return lvl == want_level, f"slot {slot_idx} level read={lvl} want={want_level}"
    return _check


def diff_dump(edited_path: Path, dump_path: Path):
    """Independent, file-level cross-check of gb_roundtrip's own SRAM figures: read the
    dump and the file we actually fed the emulator, and compare their SRAM spans
    (split_rtc_tail — the RTC footer is mGBA's clock, never diffed as save data). A length
    change here is a hard failure regardless of what secondary_gate() found."""
    edited = edited_path.read_bytes()
    dump = dump_path.read_bytes()
    sram_len, _ = grt.split_rtc_tail(len(edited))
    dump_sram_len, _ = grt.split_rtc_tail(len(dump))
    if dump_sram_len != sram_len:
        return False, f"SRAM length {sram_len} -> {dump_sram_len}", 0
    changed = sum(1 for a, b in zip(edited[:sram_len], dump[:sram_len]) if a != b)
    return True, "length unchanged", changed


# --------------------------------------------------------------------------
# one save's whole case list
# --------------------------------------------------------------------------
class Tally:
    def __init__(self, sav_name):
        self.sav_name = sav_name
        self.ok = self.fail = self.skip = 0

    def record(self, case, ok, detail=""):
        tag = "[ok]  " if ok else "[FAIL]"
        print(f"{tag} {self.sav_name} {case}" + (f" -- {detail}" if detail else ""))
        if ok:
            self.ok += 1
        else:
            self.fail += 1
        return ok

    def skip_case(self, case, reason):
        print(f"[skip] {self.sav_name} {case} -- {reason}")
        self.skip += 1


def edited_case(python, vendor, work, rom, gen, edited_sav, orig_size, dump_name,
               extra_args, tally, label, extra_check=None):
    """Boot an edited save with content assertions, THEN run the secondary checks.
    `extra_check`, if given, is a callable(rep) -> (bool, str) for an assertion
    gb_roundtrip's own --expect* flags cannot express (slot-anchoring: see
    check_nick_slot/check_level_slot). Returns the combined bool so callers can gate
    follow-on steps on it.

    LENGTH GUARD (trap 8, docs/GEN12-EDIT-DESIGN.md sec. 5) runs BEFORE the boot, not
    after: an engine bug that drops the 48-byte RTC footer (or otherwise changes the
    file's length) on one op was proven to stay green here -- gb_roundtrip's own
    harness_error only fires on a SRAM-length mismatch measured INSIDE the emulator,
    which never runs if the edited file itself is already the wrong size for a reason
    unrelated to what the emulator does with it."""
    actual = edited_sav.stat().st_size
    if actual != orig_size:
        return tally.record(label, False,
            f"length guard: {edited_sav.name} is {actual}B, the original save is "
            f"{orig_size}B -- refusing to boot a save whose length already changed "
            "before the emulator ever saw it")
    dump_path = work / f"{dump_name}_dump.sav"
    rc, rep, out, err = boot(python, rom, edited_sav, work / dump_name, vendor,
                             work / f"{dump_name}.json", dump_path=dump_path,
                             extra_args=extra_args)
    content_ok = (rc == 0)
    extra_ok, extra_detail = (True, "")
    if extra_check is not None:
        extra_ok, extra_detail = extra_check(rep)
    sec_ok, sec_detail = secondary_gate(rep, gen)
    diff_ok, diff_detail, changed = (True, "no dump", 0)
    if dump_path.exists():
        diff_ok, diff_detail, changed = diff_dump(edited_sav, dump_path)
    fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
    detail = (f"verdict={rep.get('verdict')} extra=({extra_detail}) sec=({sec_detail}) "
             # the whole SRAM span INCLUDING scratch (Gen 1: sprite buffers 0x0000-0x0497;
             # Gen 2: sDecompressScratch + the window stack) -- informational only, NEVER
             # asserted on: it churns by design and its size depends only on where the
             # boot happened to stop.
             f"dump=({diff_detail}, {changed}B changed incl. scratch, not asserted)")
    ok = content_ok and extra_ok and sec_ok and diff_ok
    if not ok:
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    return tally.record(label, ok, detail)


def run_mirror_case(name, info, rom, sav, work, binary, python, vendor, tally, base_name):
    """BACKLOG #49 P0 — the regression test for source/gen2_save.c's k_gs_mirror fix
    (0x3D69 -> 0x3D96). The module docstring above already describes doing this by hand
    once (a reviewer corrupting a Crystal save's primary checksum after an edit); this
    formalizes the SAME experiment for Gold/Silver, the generation whose mirror address
    was actually wrong.

    Unlike tools/gb_roundtrip.py --selftest's version of this proof (which hand-builds a
    candidate backup from the primary), this one edits through this tree's OWN write path
    — the surgery tool, same gbs_commit_list -> g2w_commit_list -> write_patch /
    refresh_checksums every other case in this file uses — so the backup being tested is
    one PokeDNA actually produced, not a synthetic stand-in. Then it corrupts ONLY the
    stored PRIMARY checksum (simulating damage AFTER that commit: a bad SD sector, a torn
    write from something else entirely) and boots.

    Pre-fix, this scenario was unrecoverable: g2w_finish() computed the stored BACKUP
    checksum over the wrong span (k_gs_mirror[1].dest == 0x3D69), and the design doc's
    ROM-boot experiment proved that map is the one the game REJECTS ("The save file is /
    corrupted!") — so a G/S save PokeDNA had touched could never survive a damaged
    primary. Post-fix the game must load the backup PokeDNA wrote and show the edit."""
    if info["gen"] != 2:
        return
    off = PRIMARY_CKSUM_OFF["gs" if name == "gold" else "crystal"]

    committed = work / "mirror_committed.sav"
    rc, out, err = run_surgery(binary, sav, committed,
                               [["nick", "party", "0", MIRROR_NICK]])
    if rc != 0:
        tally.record("G/S backup rescue (mirror, BACKLOG #49 P0)", False,
                    f"surgery refused: {err.strip()}")
        return

    data = bytearray(committed.read_bytes())
    data[off] ^= 0xFF
    data[off + 1] ^= 0xFF                      # flip the stored PRIMARY checksum only
    corrupted = work / "mirror_corrupted.sav"
    corrupted.write_bytes(bytes(data))

    rc, rep, out, err = boot(python, rom, corrupted, work / "mirror", vendor,
                             work / "mirror.json",
                             extra_args=["--expect", "accept",
                                         "--expect-name", base_name,
                                         "--expect-party", MIRROR_NICK])
    slot_ok, slot_detail = check_nick_slot(0, MIRROR_NICK)(rep)
    ok = (rc == 0) and slot_ok
    detail = (f"verdict={rep.get('verdict')} {slot_detail} -- primary checksum corrupted "
             f"AFTER a real PokeDNA commit; the game must recover from the backup that "
             f"commit wrote")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("G/S backup rescue (mirror, BACKLOG #49 P0)", ok, detail)


def run_money_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #49 P0's field-write primitive, proven the same way every other case here
    is: a REAL edit through the surgery tool's new `--op money`, which is
    gbs_write_field() + gbs_finish() and nothing else -- not a Pokemon op -- booted, and
    checked against what the GAME shows. Unlike every content check above (screen-
    scraped text), money is not readable off any screen this driver captures, so this is
    read straight off WRAM (docs/GEN12-PARITY-DESIGN.md §4.2: "screen scraping cannot
    see money or a fly bit"), which is what --read-mem is for. On Gen 2 the read is
    gated on SVBK already being bank 0 or 1 (§4.2 rule 1) -- gb_roundtrip.py's own
    read_mem plumbing computes and reports that, this case only has to check it."""
    edited = work / "money.sav"
    rc, out, err = run_surgery(binary, sav, edited, [["money", str(MONEY_VALUE)]])
    if rc != 0:
        tally.record("money (field write, BACKLOG #49 P0)", False,
                    f"surgery refused: {err.strip()}")
        return

    addr = MONEY_WRAM[name]
    want = _bcd24_hex(MONEY_VALUE) if info["gen"] == 1 else _be24_hex(MONEY_VALUE)
    rc, rep, out, err = boot(python, rom, edited, work / "money", vendor,
                             work / "money.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:{MONEY_LEN}"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    money_ok = svbk_ok and got == want
    ok = (rc == 0) and money_ok
    detail = (f"verdict={rep.get('verdict')} svbk={mem.get('svbk')} svbk_ok={svbk_ok} "
             f"{addr:#06x}={got!r} want={want!r}")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("money (field write, BACKLOG #49 P0)", ok, detail)


# BACKLOG #126c — wStatusFlags/wPlayerGender WRAM anchors, one .sym lookup each,
# same posture as MONEY_WRAM above: --op statusflags/--op gender (tests/
# host_gbsurgery_tool.c's do_statusflags/do_gender, BACKLOG #96 D10/Kris) already
# write the SAVE-FILE bytes and were already proven on the host, but neither one had
# a gate case proving the write reaches a BOOTED game's WRAM.
#   Gold:    wStatusFlags = bank 01, $D571 (assets/upstream/pokegold/symbols/
#            pokegold.sym:42364)
#   Crystal: wStatusFlags = bank 01, $D84C (assets/upstream/pokecrystal/symbols/
#            pokecrystal.sym:57091)
# Gen 1 has no equivalent field (do_statusflags refuses it outright) -- no "red"/
# "yellow" entries, same absence shape HELDITEM_WRAM already has for Gen 1.
STATUSFLAGS_VALUE = 0x05   # distinct bit pattern (bits 0+2), not a trivial 0/1 that
                           # could accidentally match a real save's own flags byte
STATUSFLAGS_WRAM = {"gold": 0xD571, "crystal": 0xD84C}

# wPlayerGender is Crystal-only (Gold/Silver's card has no gender concept, source/
# gb_fields.c's own GBF_GENDER row is 0 for both G1 rows and the GS row) -- symbol:
#   Crystal: wPlayerGender = bank 01, $D472 (assets/upstream/pokecrystal/symbols/
#            pokecrystal.sym:56364)
GENDER_VALUE = 1   # female (Kris) -- 0 is every save's own default, so 1 is the one
                   # value that PROVES the write reached the booted game
GENDER_WRAM = {"crystal": 0xD472}


def run_statusflags_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #126c: --op statusflags's write (do_statusflags, BACKLOG #96 D10),
    proven the same shape as run_money_case -- boot it and read wStatusFlags back off
    WRAM. Gen 2 only (Gold and Crystal); Gen 1 has no such field and the surgery tool
    itself refuses before there is anything to boot, same posture run_helditem_case's
    own Gen-1 skip already has."""
    if info["gen"] == 1:
        tally.skip_case("statusflags (field write, BACKLOG #126c)",
                        "Gen 1 has no GBF_STATUS_FLAGS field; do_statusflags refuses it")
        return
    edited = work / "statusflags.sav"
    rc, out, err = run_surgery(binary, sav, edited, [["statusflags", str(STATUSFLAGS_VALUE)]])
    if rc != 0:
        tally.record("statusflags (field write, BACKLOG #126c)", False,
                    f"surgery refused: {err.strip()}")
        return

    addr = STATUSFLAGS_WRAM[name]
    want = f"{STATUSFLAGS_VALUE:02x}"
    rc, rep, out, err = boot(python, rom, edited, work / "statusflags", vendor,
                             work / "statusflags.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:1"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    ok = (rc == 0) and svbk_ok and got == want
    detail = (f"verdict={rep.get('verdict')} svbk={mem.get('svbk')} svbk_ok={svbk_ok} "
             f"{addr:#06x}={got!r} want={want!r}")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("statusflags (field write, BACKLOG #126c)", ok, detail)


def run_gender_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #126c: --op gender's write (do_gender, BACKLOG #96 Kris), proven the
    same shape as run_money_case -- boot it and read wPlayerGender back off WRAM.
    Crystal only: Gold/Silver's card has no gender concept and do_gender refuses
    anything that is not a Crystal session outright (same "the surgery step itself
    refuses before there is anything to boot" posture run_helditem_case's own Gen-1
    skip already documents); Red/Yellow skip for the same reason."""
    if name != "crystal":
        tally.skip_case("gender (field write, BACKLOG #126c)",
                        "gender is Crystal only; do_gender refuses every other game")
        return
    edited = work / "gender.sav"
    rc, out, err = run_surgery(binary, sav, edited, [["gender", str(GENDER_VALUE)]])
    if rc != 0:
        tally.record("gender (field write, BACKLOG #126c)", False,
                    f"surgery refused: {err.strip()}")
        return

    addr = GENDER_WRAM[name]
    want = f"{GENDER_VALUE:02x}"
    rc, rep, out, err = boot(python, rom, edited, work / "gender", vendor,
                             work / "gender.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:1"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    ok = (rc == 0) and svbk_ok and got == want
    detail = (f"verdict={rep.get('verdict')} svbk={mem.get('svbk')} svbk_ok={svbk_ok} "
             f"{addr:#06x}={got!r} want={want!r}")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("gender (field write, BACKLOG #126c)", ok, detail)


# BACKLOG #89 — the Hall of Fame gate: gbh_clear()/gbh_set_count() proven the same way
# money is above (a real edit through --op hofclear/hofcount, booted, read straight off
# WRAM). wNumHoFTeams/wHallOfFameCount's WRAM addresses are the SAME symbol addresses
# gen_gbfields.py's own D() derivation resolved GBF_HOF_COUNT's FILE offset from (RED
# bank00:d5a2, YELLOW bank00:d5a1, GS bank01:d683, CRYSTAL bank01:d95e) — the save-file
# offset and this WRAM address are two different numbers for the same field (file =
# region_base + (wram - region_start)), so this case is checking the count the BOOTED
# GAME itself holds, not just re-reading the .sav this tool wrote.
HOF_COUNT_WRAM = {"red": 0xD5A2, "yellow": 0xD5A1, "gold": 0xD683, "crystal": 0xD95E}
HOF_COUNT_LEN = 1
HOF_SET_COUNT_VALUE = 3


def run_hofclear_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """gbh_clear(): erases every recorded team + the lifetime counter. The count is
    what actually gates bills_pc.asm/main_menu.asm's own "HALL OF FAME" PC option
    (source/gb_hof.h's header, re-derived from the pinned pokered decomp) — reading it
    back at 0 off the BOOTED game's WRAM is the strongest proof available without
    OCR'ing the PC's own menu text (which this harness's screen-scrape does not cover)."""
    edited = work / "hofclear.sav"
    rc, out, err = run_surgery(binary, sav, edited, [["hofclear"]])
    if rc != 0:
        tally.record("hofclear (BACKLOG #89)", False, f"surgery refused: {err.strip()}")
        return

    addr = HOF_COUNT_WRAM[name]
    rc, rep, out, err = boot(python, rom, edited, work / "hofclear", vendor,
                             work / "hofclear.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:{HOF_COUNT_LEN}"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    ok = (rc == 0) and svbk_ok and got == "00"
    detail = (f"verdict={rep.get('verdict')} svbk={mem.get('svbk')} svbk_ok={svbk_ok} "
             f"{addr:#06x}={got!r} want='00'")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("hofclear (BACKLOG #89)", ok, detail)


def run_hofcount_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """gbh_set_count(3): the SET COUNT screen action, read back off the same WRAM
    address the clear case above uses.

    BACKLOG #89 D1: on Gen 1, gbh_set_count() now clamps to the teams actually
    present in THIS .sav (never past GBH_G1_CAPACITY), not a flat 255 -- and the
    real corpus varies (Red: 9 teams, so 3 lands untouched; Yellow: 1 team, so 3
    clamps to 1). --op hofcount prints the real post-clamp count on its own stdout
    (do_hofcount, tests/host_gbsurgery_tool.c) specifically so this case can check
    against what the write ACTUALLY did, not the token handed to it."""
    edited = work / "hofcount.sav"
    rc, out, err = run_surgery(binary, sav, edited,
                               [["hofcount", str(HOF_SET_COUNT_VALUE)]])
    if rc != 0:
        tally.record("hofcount (BACKLOG #89)", False, f"surgery refused: {err.strip()}")
        return

    m = re.search(r"hofcount result: (\d+)", out)
    if not m:
        tally.record("hofcount (BACKLOG #89)", False,
                     f"surgery gave no 'hofcount result: N' line to check against "
                     f"-- stdout: {out.strip()!r}")
        return
    actual_count = int(m.group(1))

    addr = HOF_COUNT_WRAM[name]
    want = f"{actual_count:02x}"
    rc, rep, out, err = boot(python, rom, edited, work / "hofcount", vendor,
                             work / "hofcount.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:{HOF_COUNT_LEN}"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    ok = (rc == 0) and svbk_ok and got == want
    detail = (f"verdict={rep.get('verdict')} svbk={mem.get('svbk')} svbk_ok={svbk_ok} "
             f"{addr:#06x}={got!r} want={want!r} "
             f"(requested {HOF_SET_COUNT_VALUE}, real post-clamp {actual_count})")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("hofcount (BACKLOG #89)", ok, detail)


def run_hofappend_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """gbh_append_team() (BACKLOG #194 F3): one canned 1-mon team appended to the
    corpus save via --op hofappend (do_hofappend, tests/host_gbsurgery_tool.c),
    read back the SAME way run_hofcount_case() does -- the lifetime count off a
    REAL booted Red/Gold/Crystal's own WRAM, not just the host test's in-memory
    image. Proves the append (and, on Gen 1, a full-table eviction shift, though
    none of Guy's corpus saves are anywhere near 50 teams so this case only ever
    exercises the "not yet full" branch -- the eviction branch is proven by
    tests/host_gbhof_test.c's own append_gen1_boundary(), a real boot cannot
    reach 50 real HoF wins without literally playing the game that many times)
    reaches the card the same way gbh_clear()/gbh_set_count() already do."""
    edited = work / "hofappend.sav"
    rc, out, err = run_surgery(binary, sav, edited, [["hofappend"]])
    if rc != 0:
        tally.record("hofappend (BACKLOG #194)", False, f"surgery refused: {err.strip()}")
        return

    m = re.search(r"hofappend result: (\d+)", out)
    if not m:
        tally.record("hofappend (BACKLOG #194)", False,
                     f"surgery gave no 'hofappend result: N' line to check against "
                     f"-- stdout: {out.strip()!r}")
        return
    actual_count = int(m.group(1))

    addr = HOF_COUNT_WRAM[name]
    want = f"{actual_count:02x}"
    rc, rep, out, err = boot(python, rom, edited, work / "hofappend", vendor,
                             work / "hofappend.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:{HOF_COUNT_LEN}"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    ok = (rc == 0) and svbk_ok and got == want
    detail = (f"verdict={rep.get('verdict')} svbk={mem.get('svbk')} svbk_ok={svbk_ok} "
             f"{addr:#06x}={got!r} want={want!r} (post-append count {actual_count})")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("hofappend (BACKLOG #194)", ok, detail)


def run_hofdelete_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """gbh_delete_team(0) (BACKLOG #194 F3): deletes the newest team off the SAME
    edited save run_hofappend_case() just produced (chained, not the raw corpus
    save -- so this case proves the delete on top of a real append, the shape the
    screen's own DELETE TEAM row is reached in), read back the same WRAM count
    address once more; the count must be back down to the ORIGINAL corpus value
    (append then delete is a no-op on the count -- tests/host_gbhof_test.c's own
    delete_inverse() already proves this byte-exact off-card; this case only
    proves the count-field half of it survives a real boot)."""
    appended = work / "hofappend.sav"
    if not appended.exists():
        # Independent re-run (case ordering safety): re-produce the same edited
        # save this case chains onto rather than assume run_hofappend_case() ran
        # first in THIS process (tally order is fixed below, but a future re-order
        # should not silently misfire on a missing file).
        rc, out, err = run_surgery(binary, sav, appended, [["hofappend"]])
        if rc != 0:
            tally.record("hofdelete (BACKLOG #194)", False,
                         f"setup (hofappend) refused: {err.strip()}")
            return

    edited = work / "hofdelete.sav"
    rc, out, err = run_surgery(binary, appended, edited, [["hofdelete"]])
    if rc != 0:
        tally.record("hofdelete (BACKLOG #194)", False, f"surgery refused: {err.strip()}")
        return

    m = re.search(r"hofdelete result: (\d+)", out)
    if not m:
        tally.record("hofdelete (BACKLOG #194)", False,
                     f"surgery gave no 'hofdelete result: N' line to check against "
                     f"-- stdout: {out.strip()!r}")
        return
    actual_count = int(m.group(1))

    addr = HOF_COUNT_WRAM[name]
    want = f"{actual_count:02x}"
    rc, rep, out, err = boot(python, rom, edited, work / "hofdelete", vendor,
                             work / "hofdelete.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:{HOF_COUNT_LEN}"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    ok = (rc == 0) and svbk_ok and got == want
    detail = (f"verdict={rep.get('verdict')} svbk={mem.get('svbk')} svbk_ok={svbk_ok} "
             f"{addr:#06x}={got!r} want={want!r} (post-append-then-delete count "
             f"{actual_count}, expected back at the original corpus count)")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("hofdelete (BACKLOG #194)", ok, detail)


# BACKLOG #85/#86/#90/#94 — WRAM anchors, one .sym lookup each, same posture as
# MONEY_WRAM above (a live symbol address, not a save-file offset): the save-file
# offsets these four cores already use (source/gb_fields.c) live in SRAM/the file;
# --read-mem reads the RUNNING GAME'S WRAM, which is a different address space
# entirely and has to be resolved from the .sym files on its own, exactly like
# MONEY_WRAM/BADGE_WRAM/NAME_WRAM already are.
DAYCARE_FLAG_WRAM = {"red": 0xDA48, "yellow": 0xDA47, "gold": 0xDC40, "crystal": 0xDEF5}
FLY_FLAGS_WRAM = {"red": 0xD70B, "yellow": 0xD70A, "gold": 0xD9EE, "crystal": 0xDCA5}
FLY_FLAGS_LEN = {"red": 2, "yellow": 2, "gold": 4, "crystal": 4}

# BACKLOG #88 -- wEventFlags' own WRAM address per game (pokered.sym:19395 $D747,
# pokegold.sym:42451 $D7B7, pokecrystal.sym:57227 $DA72; Yellow shares Red's own
# main_data layout, §1.10, so its wEventFlags sits one byte earlier the same way
# wMainDataStart does, $D746) -- a DIFFERENT address space from the save-FILE offset
# gb_fields.c's GBF_EVENT_FLAGS_BASE(_G2) already resolves (same split as every other
# WRAM anchor above). SAV_FLAGS_FILE_BASE is that save-file offset, used ONLY to read
# the CURRENT bit straight out of the .sav bytes before editing (so this case never
# repeats the fly-case D6 bug of "editing a bit that was already set" -- 0 bytes
# written, a false accept).
EVENT_FLAGS_WRAM = {"red": 0xD747, "yellow": 0xD746, "gold": 0xD7B7, "crystal": 0xDA72}
SAV_FLAGS_FILE_BASE = {"red": 0x29F3, "yellow": 0x29F3, "gold": 0x261F, "crystal": 0x2600}
# EVENT_MADE_UNOWN_APPEAR_IN_RUINS (Gen 2, event_flags.asm:55) -- index 46 on BOTH
# Gold/Silver and Crystal (re-derived independently by tools/gen_gbfields.py's own
# self-test, not copied from the research doc). EVENT_GOT_TOWN_MAP (Gen 1,
# event_constants.asm) -- index 24 on Red/Yellow, "a harmless Gen-1 flag the research
# names" (docs/briefs/88-gb-flags-brief.md's own gate-case instruction, §15's Key
# events row) -- toggling it does not gate any NPC position or map state.
FLAGS_CASE_INDEX = {"red": 24, "yellow": 24, "gold": 46, "crystal": 46}
FLAGS_CASE_NAME = {"red": "EVENT_GOT_TOWN_MAP", "yellow": "EVENT_GOT_TOWN_MAP",
                   "gold": "EVENT_MADE_UNOWN_APPEAR_IN_RUINS",
                   "crystal": "EVENT_MADE_UNOWN_APPEAR_IN_RUINS"}
BOXNAMES_WRAM = {"gold": 0xD8BF, "crystal": 0xDB75}          # Gen 1: no box names, no case
# D6 (BACKLOG #88 review): the COUNTERS tab's Gen-2 "lucky number already shown today"
# flag (GBF_LUCKY_NUMBER_SHOW_FLAG, --op counter lucky) as a clean boot-verifiable
# WRAM byte -- wLuckyNumberShowFlag, bank 01, verified against each game's own .sym
# (assets/upstream/pokegold/symbols/pokegold.sym:42501 "01:d9e7 wLuckyNumberShowFlag",
# assets/upstream/pokecrystal/symbols/pokecrystal.sym:57291 "01:dc9d
# wLuckyNumberShowFlag"). Gen 1 has no lucky-number system at all (--op counter lucky
# refuses outright, GBF_LUCKY_NUMBER_SHOW_FLAG's Gen-1 gbf_off() is 0) -- Gold/Crystal
# only, same posture as clock/boxname.
LUCKY_WRAM = {"gold": 0xD9E7, "crystal": 0xDC9D}

# BACKLOG #87 item 6 — the Pokedex owned/seen WRAM anchors, one .sym lookup each per
# game (wPokedexOwned/wPokedexSeen Gen 1; wPokedexCaught/wPokedexSeen Gen 2 -- Gen 2's
# decomps name the "owned" field "Caught", not "Owned"; same field gb_fields.c's
# GBF_DEX_OWNED tracks). Lengths match gb_fields.c's own per-generation field width
# (19 B Gen 1, 32 B Gen 2).
DEX_OWNED_WRAM = {"red": 0xD2F7, "yellow": 0xD2F6, "gold": 0xDBE4, "crystal": 0xDE99}
DEX_SEEN_WRAM  = {"red": 0xD30A, "yellow": 0xD309, "gold": 0xDC04, "crystal": 0xDEB9}
DEX_FIELD_LEN  = {"red": 19, "yellow": 19, "gold": 32, "crystal": 32}

# BACKLOG #87 D4 -- the Unown-dex GATE, Gen 2 only: wStatusFlags (bit 1 =
# STATUSFLAGS_UNOWN_DEX_F) and wFirstUnownSeen, straight from pokegold.sym/
# pokecrystal.sym (wStatusFlags 01:d571/01:d84c, wFirstUnownSeen 01:dc3f/01:def4 --
# the same two symbols tools/gen_gbfields.py's GBF_STATUS_FLAGS/GBF_FIRST_UNOWN_SEEN
# rows derive their SAV-file offsets from, this dict is the LIVE WRAM side of the
# same two facts).
STATUS_FLAGS_WRAM = {"gold": 0xD571, "crystal": 0xD84C}
FIRST_UNOWN_SEEN_WRAM = {"gold": 0xDC3F, "crystal": 0xDEF4}


def run_daycare_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #85, via gb_daycare.h's gbd_deposit -- --op daycare 0 <dex> deposits a
    fixed-stat test mon into day-care slot 0 (the Day-Care Man; Gen 1's only slot) and
    checks the occupancy bit in WRAM. Not screen-scraped: gb_roundtrip.py has no
    day-care-screen reader (the design doc's own §4.2 rule -- only a title/party
    screen this driver can read), so this is read straight off WRAM like money is.
    Gen 1: gbd_deposit writes the WHOLE flag byte to 1, so the check is an exact
    match. Gen 2: it only ever SETS bit 0 (read-modify-write, gb_daycare.c's own
    discipline), so the check masks for that bit rather than assuming the baseline
    byte was 0 -- Guy's own Gold.sav shows the corpus baseline can already hold other
    bits (the intro-seen bit, gb_daycare.h's own header note)."""
    edited = work / "daycare.sav"
    rc, out, err = run_surgery(binary, sav, edited, [["daycare", "0", "25"]])   # dex 25 = Pikachu
    if rc != 0:
        tally.record("daycare (BACKLOG #85)", False, f"surgery refused: {err.strip()}")
        return

    addr = DAYCARE_FLAG_WRAM[name]
    rc, rep, out, err = boot(python, rom, edited, work / "daycare", vendor,
                             work / "daycare.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:1"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    got_val = int(got, 16) if isinstance(got, str) else None
    occupied = got_val is not None and (
        got_val == 0x01 if info["gen"] == 1 else (got_val & 0x01) == 0x01)
    ok = (rc == 0) and svbk_ok and occupied
    detail = (f"verdict={rep.get('verdict')} svbk_ok={svbk_ok} {addr:#06x}={got!r} "
             f"occupied={occupied}")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("daycare (BACKLOG #85)", ok, detail)


CLOCK_HHOURS_HRAM = {"gold": 0xFF96, "crystal": 0xFF94}   # Gen 1: no clock, no case
# Fixed so the two boots below (baseline / +2h-shifted) see the SAME hardware RTC
# reading and only the offset shift can move hHours -- an unpinned (real-time) RTC
# would make the delta depend on wall-clock skew between the two mGBA runs.
CLOCK_RTC_PIN = "2026-01-01 12:00:00"


def run_clock_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #86, P1a review D1/D8 -- gb_clock.h no longer claims the in-game clock is
    an absolute time it can set (it is hardware RTC + a stored offset, see gb_clock.h's
    header note), so this case asserts on what the GAME's own FixTime computes, not on
    the offset bytes: boot the UNTOUCHED save and the +2h-SHIFTED save under the exact
    same pinned --rtc and read hHours (home/time.asm's FixTime output) off HRAM in both;
    (after - before) mod 24 must be exactly 2. A second sub-case proves --op clockreset:
    the main menu's own continue-game-info row prints the literal "TIME NOT SET" string
    (engine/menus/main_menu.asm) once sRTCStatusFlags reads RTC_RESET, which
    gb_roundtrip.py's main-menu scrape (rep["main_menu"]) already captures."""
    addr = CLOCK_HHOURS_HRAM[name]
    min_addr = addr + 2   # hMinutes sits two bytes past hHours in FixTime's own HRAM layout

    # ---- sub-case A: +2h shift moves the GAME's own computed hour by exactly 2 -------
    rc, rep_before, out, err = boot(python, rom, sav, work / "clock_before", vendor,
                                    work / "clock_before.json",
                                    extra_args=["--expect", "accept", "--rtc", CLOCK_RTC_PIN,
                                                "--read-mem", f"{addr:#06x}:1",
                                                "--read-mem", f"{min_addr:#06x}:1"])
    before_mem = rep_before.get("mem") or {}
    before = before_mem.get(f"{addr:#06x}")
    before_val = int(before, 16) if isinstance(before, str) else None
    before_min = before_mem.get(f"{min_addr:#06x}")
    before_min_val = int(before_min, 16) if isinstance(before_min, str) else None

    shifted = work / "clock_shift.sav"
    rc2, out2, err2 = run_surgery(binary, sav, shifted, [["clockshift", "0", "2", "0", "0"]])
    if rc2 != 0:
        tally.record("clock shift (BACKLOG #86)", False, f"surgery refused: {err2.strip()}")
    else:
        rc3, rep_after, out3, err3 = boot(python, rom, shifted, work / "clock_after", vendor,
                                          work / "clock_after.json",
                                          extra_args=["--expect", "accept", "--rtc", CLOCK_RTC_PIN,
                                                      "--read-mem", f"{addr:#06x}:1"])
        after_mem = rep_after.get("mem") or {}
        after = after_mem.get(f"{addr:#06x}")
        after_val = int(after, 16) if isinstance(after, str) else None
        have_both = before_val is not None and after_val is not None
        delta_mod24 = ((after_val - before_val) % 24) if have_both else None
        delta_ok = have_both and delta_mod24 == 2
        ok = (rc == 0) and (rc3 == 0) and delta_ok
        detail = (f"before hHours@{addr:#06x}={before!r} after={after!r} "
                 f"delta_mod24={delta_mod24}")
        if not ok:
            fails = [f.strip() for f in out3.splitlines() if f.strip().startswith("FAIL:")]
            if fails:
                detail += " | " + "; ".join(fails)
            tail = stderr_tail(err3)
            if tail:
                detail += " | stderr: " + tail
        tally.record("clock shift (BACKLOG #86)", ok, detail)

    # ---- sub-case A2: a BACKWARD shift that actually BORROWS, the other direction from
    # sub-case A (P1a review D1's own "the carry chain both ways" requirement). A plain
    # -2h shift never exercised the borrow: both the Gold and Crystal test saves store
    # wStartHour == 23, so -2h lands on 21 without going negative anywhere in wrap_add's
    # own arithmetic -- the b86 review (D4) mutated wrap_add's negative branch to
    # positive-only (int32_t c = v / mod;) and this sub-case still passed. A -30 MINUTE
    # shift borrows through hMinutes into hHours instead (minutes go negative, wrap_add
    # must actually add `mod` back), so it is the case that catches that mutation.
    # Reuses `before_val`/`before_min_val` from sub-case A's own "clock_before" boot (the
    # same untouched baseline, no need to re-boot it) -- independent of whether the
    # forward shift above succeeded. */
    shifted_back = work / "clock_shift_back.sav"
    rc2b, out2b, err2b = run_surgery(binary, sav, shifted_back, [["clockshift", "0", "0", "-30", "0"]])
    if rc2b != 0:
        tally.record("clock shift backward (BACKLOG #86/#108)", False,
                     f"surgery refused: {err2b.strip()}")
    else:
        rc3b, rep_after_b, out3b, err3b = boot(python, rom, shifted_back, work / "clock_after_back",
                                               vendor, work / "clock_after_back.json",
                                               extra_args=["--expect", "accept", "--rtc", CLOCK_RTC_PIN,
                                                           "--read-mem", f"{addr:#06x}:1",
                                                           "--read-mem", f"{min_addr:#06x}:1"])
        after_mem_b = rep_after_b.get("mem") or {}
        after_b = after_mem_b.get(f"{addr:#06x}")
        after_val_b = int(after_b, 16) if isinstance(after_b, str) else None
        after_min_b = after_mem_b.get(f"{min_addr:#06x}")
        after_min_val_b = int(after_min_b, 16) if isinstance(after_min_b, str) else None
        have_both_b = (before_val is not None and after_val_b is not None and
                       before_min_val is not None and after_min_val_b is not None)
        delta_min_b = (((before_val * 60 + before_min_val) -
                        (after_val_b * 60 + after_min_val_b)) % 1440) if have_both_b else None
        delta_ok_b = have_both_b and delta_min_b == 30
        ok_b = (rc == 0) and (rc3b == 0) and delta_ok_b
        detail_b = (f"before hHours@{addr:#06x}={before!r} hMinutes@{min_addr:#06x}={before_min!r} "
                   f"after hHours={after_b!r} hMinutes={after_min_b!r} "
                   f"delta_min={delta_min_b} (want 30, i.e. -30m borrowing into hHours)")
        if not ok_b:
            fails = [f.strip() for f in out3b.splitlines() if f.strip().startswith("FAIL:")]
            if fails:
                detail_b += " | " + "; ".join(fails)
            tail = stderr_tail(err3b)
            if tail:
                detail_b += " | stderr: " + tail
        tally.record("clock shift backward (BACKLOG #86/#108)", ok_b, detail_b)

    # ---- sub-case B: clockreset makes the main menu show "TIME NOT SET" -------------
    reset = work / "clock_reset.sav"
    rc4, out4, err4 = run_surgery(binary, sav, reset, [["clockreset"]])
    if rc4 != 0:
        tally.record("clock reset (BACKLOG #86)", False, f"surgery refused: {err4.strip()}")
        return

    rc5, rep5, out5, err5 = boot(python, rom, reset, work / "clock_reset", vendor,
                                 work / "clock_reset.json",
                                 extra_args=["--expect", "accept", "--rtc", CLOCK_RTC_PIN])
    menu_rows = rep5.get("main_menu") or []
    time_not_set = any("TIME NOT SET" in row for row in menu_rows)
    ok5 = (rc5 == 0) and time_not_set
    detail5 = f"verdict={rep5.get('verdict')} main_menu={menu_rows!r} time_not_set={time_not_set}"
    if not ok5:
        fails = [f.strip() for f in out5.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail5 += " | " + "; ".join(fails)
        tail = stderr_tail(err5)
        if tail:
            detail5 += " | stderr: " + tail
    tally.record("clock reset (BACKLOG #86)", ok5, detail5)

    # ---- sub-case C: clockclear on an already-flagged save makes "TIME NOT SET"
    # disappear again (BACKLOG #86/#108) -- runs clockclear on `reset` (sub-case B's own
    # flagged save, confirmed to show the banner above) and re-boots to confirm the main
    # menu no longer prints it. gbc_clear_status_flags() only dismisses the banner (it
    # does not fix a dead battery, gb_clock.h's own header note) -- this case proves
    # exactly that dismissal, on a save this gate itself flagged, not a claim about any
    # underlying hardware RTC state. */
    cleared = work / "clock_clear.sav"
    rc6, out6, err6 = run_surgery(binary, reset, cleared, [["clockclear"]])
    if rc6 != 0:
        tally.record("clock clear (BACKLOG #86/#108)", False, f"surgery refused: {err6.strip()}")
        return

    rc7, rep7, out7, err7 = boot(python, rom, cleared, work / "clock_clear", vendor,
                                 work / "clock_clear.json",
                                 extra_args=["--expect", "accept", "--rtc", CLOCK_RTC_PIN])
    menu_rows7 = rep7.get("main_menu") or []
    still_flagged = any("TIME NOT SET" in row for row in menu_rows7)
    ok7 = (rc7 == 0) and not still_flagged
    detail7 = (f"verdict={rep7.get('verdict')} main_menu={menu_rows7!r} "
              f"still_flagged={still_flagged} (want False)")
    if not ok7:
        fails = [f.strip() for f in out7.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail7 += " | " + "; ".join(fails)
        tail = stderr_tail(err7)
        if tail:
            detail7 += " | stderr: " + tail
    tally.record("clock clear (BACKLOG #86/#108)", ok7, detail7)


def run_fly_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #90, P1a review D6/D7 -- the old case set bit 5, which every corpus save
    in this suite ALREADY had set: 0 bytes written, verdict=accept regardless of
    whether gbfy_set works at all. gbfy_count()'s own destination-count fix (D7) makes
    this checkable: Gold/Crystal bit 6 = SPAWN_ROCK_TUNNEL (pokecrystal constants/
    map_data_constants.asm), confirmed CLEAR on both corpus saves, so this case now
    FAILS outright if the surgery output is byte-identical to the input (the gate D6
    asks for) and separately confirms the booted game's own WRAM byte actually moved.
    Gen 1: every one of Red/Yellow's own 11 real destinations (bits 0..10, NUM_CITY_MAPS)
    is ALREADY visited in this corpus -- there is no clear bit below the real count to
    flip, so this case is SKIPPED for Gen 1 rather than faked with a bit past the real
    count (which gbfy_set now correctly refuses, D7's whole point)."""
    if info["gen"] == 1:
        tally.skip_case("fly (BACKLOG #90)", "every real destination (bit 0..10, "
                        "NUM_CITY_MAPS) is already visited in this corpus -- no clear "
                        "bit below the real count to prove a real change with")
        return

    bit = 6   # SPAWN_ROCK_TUNNEL -- confirmed CLEAR on both Gold.sav and Crystal.sav
    base = FLY_FLAGS_WRAM[name]
    addr = base + (bit // 8)

    edited = work / "fly.sav"
    rc, out, err = run_surgery(binary, sav, edited, [["fly", str(bit)]])
    if rc != 0:
        tally.record("fly (BACKLOG #90)", False, f"surgery refused: {err.strip()}")
        return

    identical = edited.read_bytes() == sav.read_bytes()
    if identical:
        tally.record("fly (BACKLOG #90)", False,
                    f"gate D6: surgery wrote 0 bytes for bit {bit} -- either it was "
                    "already set (corpus assumption wrong) or gbfy_set is a no-op")
        return

    rc, rep, out, err = boot(python, rom, edited, work / "fly", vendor,
                             work / "fly.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:1"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    got_val = int(got, 16) if isinstance(got, str) else None
    visited = got_val is not None and (got_val & (1 << (bit % 8))) != 0
    ok = (rc == 0) and svbk_ok and visited
    detail = f"verdict={rep.get('verdict')} svbk_ok={svbk_ok} {addr:#06x}={got!r} visited={visited} bit={bit}"
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("fly (BACKLOG #90)", ok, detail)


def _dex_popcount(hexstr):
    return bin(int(hexstr, 16)).count("1") if hexstr else 0


def run_dexset_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #87 item 6 retail-gate case: gb_dex.h's gbdex_set (via the surgery
    tool's `--op dexset DEX STATE`), proven against the real WRAM dex bytes
    (wPokedexOwned/wPokedexCaught + wPokedexSeen, from each game's own .sym) -- both
    the game's own per-species bit AND the popcount the trainer card would show.

    Guy's corpus saves are fully-completed dexes (every one of the 151/251 real
    species already owned+seen, confirmed by tests/host_gbdex_test.c's own popcount
    proof) -- there is no naturally-unseen species to catch, the same problem
    run_fly_case's own Gen-1 skip note hit. Rather than skip (the brief wants both
    directions proven on all three games), this manufactures the "not seen" state
    itself: CLEAR dex #100 first (a real, provable byte change off the corpus's own
    caught+seen baseline -- gate D6), then SET it back to caught from that genuinely-
    unseen state, which is the brief's own "not seen -> caught" narrative for the
    direction that actually matters; the clear half is "the other way" it also asks
    for.
    """
    dex = 100   # <= 151, valid on every game's own cap (Gen 1's is the tightest)
    i = dex - 1
    byte_off, bit = i // 8, i % 8
    owned_addr = DEX_OWNED_WRAM[name] + byte_off
    seen_addr = DEX_SEEN_WRAM[name] + byte_off
    owned_base, seen_base, flen = DEX_OWNED_WRAM[name], DEX_SEEN_WRAM[name], DEX_FIELD_LEN[name]

    def read_state(sav_path, label):
        rc, rep, out, err = boot(python, rom, sav_path, work / label, vendor,
                                 work / f"{label}.json",
                                 extra_args=["--expect", "accept",
                                             "--read-mem", f"{owned_addr:#06x}:1",
                                             "--read-mem", f"{seen_addr:#06x}:1",
                                             "--read-mem", f"{owned_base:#06x}:{flen}",
                                             "--read-mem", f"{seen_base:#06x}:{flen}"])
        mem = rep.get("mem") or {}
        svbk_ok = bool(mem.get("svbk_ok", True))
        owned_byte = mem.get(f"{owned_addr:#06x}")
        seen_byte = mem.get(f"{seen_addr:#06x}")
        owned_field = mem.get(f"{owned_base:#06x}")
        seen_field = mem.get(f"{seen_base:#06x}")
        st = dict(rc=rc, err=err, svbk_ok=svbk_ok,
                 owned_bit=bool(owned_byte and (int(owned_byte, 16) & (1 << bit))),
                 seen_bit=bool(seen_byte and (int(seen_byte, 16) & (1 << bit))),
                 owned_count=_dex_popcount(owned_field), seen_count=_dex_popcount(seen_field))
        return st

    base = read_state(sav, "dexset_base")
    if not (base["rc"] == 0 and base["svbk_ok"] and base["owned_bit"] and base["seen_bit"]):
        tally.record("dexset (BACKLOG #87)", False,
                    f"baseline dex #{dex} is not owned+seen on this corpus save -- "
                    f"the clear/set narrative assumes it is: {base}")
        return

    # ---- clear sub-case: dex #100 owned+seen -> both off ----
    clear_sav = work / "dexset_clear.sav"
    rc, out, err = run_surgery(binary, sav, clear_sav, [["dexset", str(dex), "0"]])
    if rc != 0:
        tally.record("dexset clear (BACKLOG #87)", False, f"surgery refused: {err.strip()}")
        return
    if clear_sav.read_bytes() == sav.read_bytes():
        tally.record("dexset clear (BACKLOG #87)", False,
                    "gate D6: surgery wrote 0 bytes clearing an owned+seen species")
        return
    ac = read_state(clear_sav, "dexset_clear")
    clear_ok = (ac["rc"] == 0 and ac["svbk_ok"] and not ac["owned_bit"] and not ac["seen_bit"]
               and ac["owned_count"] == base["owned_count"] - 1
               and ac["seen_count"] == base["seen_count"] - 1)
    tally.record("dexset clear (BACKLOG #87)", clear_ok,
                f"dex #{dex} owned={ac['owned_bit']} seen={ac['seen_bit']} "
                f"owned_count {base['owned_count']}->{ac['owned_count']} "
                f"seen_count {base['seen_count']}->{ac['seen_count']}")

    # ---- set sub-case: from the now-genuinely-unseen state, catch it ----
    set_sav = work / "dexset_set.sav"
    rc, out, err = run_surgery(binary, clear_sav, set_sav, [["dexset", str(dex), "2"]])
    if rc != 0:
        tally.record("dexset set (BACKLOG #87)", False, f"surgery refused: {err.strip()}")
        return
    if set_sav.read_bytes() == clear_sav.read_bytes():
        tally.record("dexset set (BACKLOG #87)", False,
                    "gate D6: surgery wrote 0 bytes catching a not-seen species")
        return
    aset = read_state(set_sav, "dexset_set")
    set_ok = (aset["rc"] == 0 and aset["svbk_ok"] and aset["owned_bit"] and aset["seen_bit"]
             and aset["owned_count"] == ac["owned_count"] + 1
             and aset["seen_count"] == ac["seen_count"] + 1)
    tally.record("dexset set (BACKLOG #87)", set_ok,
                f"dex #{dex} owned={aset['owned_bit']} seen={aset['seen_bit']} "
                f"owned_count {ac['owned_count']}->{aset['owned_count']} "
                f"seen_count {ac['seen_count']}->{aset['seen_count']}")


def run_unown_gate_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #87 D4 retail-gate case: the Unown-dex GATE (wStatusFlags bit 1 /
    wFirstUnownSeen) stays in sync with a DIRECT edit of dex #201 (Unown), the same
    way the real game's own UpdateUnownDex/DebugRoomMenu_PokedexDex keep it in sync
    on an actual encounter. Gen 2 only (Gen 1 has no Unown dex entry at all).

    Setup: --op unownreset forces the "never met an Unown" starting state (gate bit
    clear, wFirstUnownSeen 0, wUnownDex emptied) -- a state a real cartridge that has
    simply never visited the Ruins of Alph could genuinely be in. Then --op dexset
    201 2 (mark Unown caught) is the brief's own exact worked example: boot and read
    live WRAM, expecting the gate bit SET and wFirstUnownSeen NON-ZERO.
    """
    status_addr = STATUS_FLAGS_WRAM.get(name)
    fus_addr = FIRST_UNOWN_SEEN_WRAM.get(name)
    if status_addr is None or fus_addr is None:
        tally.skip_case("unown gate (BACKLOG #87 D4)", "Gen 1 has no Unown dex entry")
        return

    def read_state(sav_path, label):
        rc, rep, out, err = boot(python, rom, sav_path, work / label, vendor,
                                 work / f"{label}.json",
                                 extra_args=["--expect", "accept",
                                             "--read-mem", f"{status_addr:#06x}:1",
                                             "--read-mem", f"{fus_addr:#06x}:1"])
        mem = rep.get("mem") or {}
        svbk_ok = bool(mem.get("svbk_ok", True))
        status_byte = mem.get(f"{status_addr:#06x}")
        fus_byte = mem.get(f"{fus_addr:#06x}")
        status_val = int(status_byte, 16) if status_byte else None
        fus_val = int(fus_byte, 16) if fus_byte else None
        return dict(rc=rc, err=err, svbk_ok=svbk_ok, status=status_val, fus=fus_val)

    reset_sav = work / "unown_gate_reset.sav"
    rc, out, err = run_surgery(binary, sav, reset_sav, [["unownreset"]])
    if rc != 0:
        tally.record("unown gate reset (BACKLOG #87 D4)", False, f"surgery refused: {err.strip()}")
        return
    base = read_state(reset_sav, "unown_gate_reset")
    reset_ok = (base["rc"] == 0 and base["svbk_ok"] and base["status"] is not None
               and (base["status"] & 0x02) == 0 and base["fus"] == 0)
    tally.record("unown gate reset (BACKLOG #87 D4)", reset_ok,
                f"after unownreset: status={base['status']:#04x} fus={base['fus']}"
                if base["status"] is not None else f"read failed: {base}")
    if not reset_ok:
        return

    dexset_sav = work / "unown_gate_dexset.sav"
    rc, out, err = run_surgery(binary, reset_sav, dexset_sav, [["dexset", "201", "2"]])
    if rc != 0:
        tally.record("unown gate dexset (BACKLOG #87 D4)", False, f"surgery refused: {err.strip()}")
        return
    after = read_state(dexset_sav, "unown_gate_dexset")
    gate_ok = (after["rc"] == 0 and after["svbk_ok"] and after["status"] is not None
              and (after["status"] & 0x02) != 0 and after["fus"] not in (None, 0))
    tally.record("unown gate dexset (BACKLOG #87 D4)", gate_ok,
                f"dexset 201 2: status {base['status']:#04x}->{after['status']:#04x} "
                f"fus {base['fus']}->{after['fus']} (want bit1 set, fus != 0)")

    # ---- R1 (b87 fix pass 2, mutation-proven DO-NOT-SHIP finding) ----
    # No --op unownreset here on purpose: Guy's own corpus saves already have an
    # Unown letter recorded (wFirstUnownSeen != 0), the exact "already has a
    # letter" state the original D4 fix's `on` branch skipped re-arming the gate
    # for. `dexset 201 0` (Wipe/none, the same shape a bulk Wipe ALL + the dex-201
    # cell takes) clears the gate; `dexset 201 2` (Catch/caught again, the same
    # shape an Undo restoring it takes) must put the gate bit back to its exact
    # pre-wipe baseline -- pre-fix it stayed OFF permanently on a save that had
    # ever met an Unown.
    baseline = read_state(sav, "unown_gate_baseline")
    baseline_ok = (baseline["rc"] == 0 and baseline["svbk_ok"] and baseline["status"] is not None
                  and (baseline["status"] & 0x02) != 0)
    tally.record("unown gate R1 baseline (BACKLOG #87 D4)", baseline_ok,
                f"corpus save as-is: status={baseline['status']:#04x} fus={baseline['fus']} "
                f"(want bit1 already set -- this save has met an Unown)"
                if baseline["status"] is not None else f"read failed: {baseline}")
    if not baseline_ok:
        return

    wipe_sav = work / "unown_gate_r1_wipe.sav"
    rc, out, err = run_surgery(binary, sav, wipe_sav, [["dexset", "201", "0"]])
    if rc != 0:
        tally.record("unown gate R1 wipe (BACKLOG #87 D4)", False, f"surgery refused: {err.strip()}")
        return
    wiped = read_state(wipe_sav, "unown_gate_r1_wipe")
    wipe_ok = (wiped["rc"] == 0 and wiped["svbk_ok"] and wiped["status"] is not None
              and (wiped["status"] & 0x02) == 0)
    tally.record("unown gate R1 wipe (BACKLOG #87 D4)", wipe_ok,
                f"dexset 201 0: status {baseline['status']:#04x}->{wiped['status']:#04x} (want bit1 clear)")
    if not wipe_ok:
        return

    recatch_sav = work / "unown_gate_r1_recatch.sav"
    rc, out, err = run_surgery(binary, wipe_sav, recatch_sav, [["dexset", "201", "2"]])
    if rc != 0:
        tally.record("unown gate R1 restore (BACKLOG #87 D4)", False, f"surgery refused: {err.strip()}")
        return
    restored = read_state(recatch_sav, "unown_gate_r1_recatch")
    restore_ok = (restored["rc"] == 0 and restored["svbk_ok"]
                 and restored["status"] == baseline["status"])
    tally.record("unown gate R1 restore (BACKLOG #87 D4)", restore_ok,
                f"dexset 201 2 (no unownreset first): status {wiped['status']:#04x}->"
                f"{restored['status']:#04x}, want back to baseline {baseline['status']:#04x} "
                f"({'MATCH' if restore_ok else 'MISMATCH -- gate stuck off, R1 regression'})")


def run_boxname_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #94, via gb_boxnames.h's gbbn_rename -- Gen 2 only (gbbn_rename refuses
    Gen 1, and this function is never called for a Gen-1 GAMES entry). Renames box 0
    to "GATE" and checks the first 4 GB-encoded bytes in WRAM ('A'=0x80, so
    "GATE" -> 86 80 94 84 -- G=0x86, A=0x80, T=0x93, E=0x84)."""
    edited = work / "boxname.sav"
    rc, out, err = run_surgery(binary, sav, edited, [["boxname", "0", "GATE"]])
    if rc != 0:
        tally.record("boxname (BACKLOG #94)", False, f"surgery refused: {err.strip()}")
        return

    addr = BOXNAMES_WRAM[name]
    want = "86809384"   # G A T E, GB charset (0x80 = 'A')
    rc, rep, out, err = boot(python, rom, edited, work / "boxname", vendor,
                             work / "boxname.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:4"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    ok = (rc == 0) and svbk_ok and got == want
    detail = f"verdict={rep.get('verdict')} svbk_ok={svbk_ok} {addr:#06x}={got!r} want={want!r}"
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("boxname (BACKLOG #94)", ok, detail)


def run_flags_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #88 -- proves --op flagset's write (gbfl_set, source/gb_flags_rw.c)
    reaches the BOOTED game's own wEventFlags, not just the .sav bytes on disk. Reads
    the CURRENT bit straight out of the corpus .sav first (the fly-case D6 lesson: an
    already-set bit makes surgery write 0 bytes and the gate would pass vacuously) and
    flips it the OTHER way -- so this case is correct regardless of which way the
    corpus save happens to have it, unlike a hardcoded "set to 1" assumption."""
    idx = FLAGS_CASE_INDEX[name]
    file_base = SAV_FLAGS_FILE_BASE[name]
    byte_off = file_base + (idx // 8)
    bit = idx % 8

    raw = sav.read_bytes()
    cur = (raw[byte_off] >> bit) & 1
    want_val = 0 if cur else 1

    edited = work / "flags.sav"
    rc, out, err = run_surgery(binary, sav, edited, [["flagset", str(idx), str(want_val)]])
    if rc != 0:
        tally.record(f"flags (BACKLOG #88, {FLAGS_CASE_NAME[name]})", False,
                    f"surgery refused: {err.strip()}")
        return

    identical = edited.read_bytes() == raw
    if identical:
        tally.record(f"flags (BACKLOG #88, {FLAGS_CASE_NAME[name]})", False,
                    f"gate D6 class: surgery wrote 0 bytes for index {idx} (cur={cur} "
                    f"want={want_val}) -- flagset is a no-op")
        return

    addr = EVENT_FLAGS_WRAM[name] + (idx // 8)
    rc, rep, out, err = boot(python, rom, edited, work / "flags", vendor,
                             work / "flags.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:1"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    got_val = int(got, 16) if isinstance(got, str) else None
    got_bit = got_val is not None and ((got_val >> bit) & 1) == want_val
    ok = (rc == 0) and svbk_ok and got_bit
    detail = (f"verdict={rep.get('verdict')} svbk_ok={svbk_ok} {addr:#06x}={got!r} "
             f"index={idx} bit_in_byte={bit} cur={cur} want={want_val} matched={got_bit}")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record(f"flags (BACKLOG #88, {FLAGS_CASE_NAME[name]})", ok, detail)


LUCKY_FILE_OFF = {"gold": 0x284F, "crystal": 0x282B}   # source/gb_fields.c's own
                                                        # GBF_LUCKY_NUMBER_SHOW_FLAG cells


def run_counter_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """D6 (BACKLOG #88 review): --op counter lucky <0|1> writes
    GBF_LUCKY_NUMBER_SHOW_FLAG (gb_fields.c) through gbs_write_field + gbs_finish, the
    COUNTERS tab's own write path for the "already shown today" byte -- proves it
    reaches the BOOTED game's own wLuckyNumberShowFlag (LUCKY_WRAM), not just the .sav
    bytes on disk. Same read-current-flip discipline as run_flags_case (the fly-case
    D6 lesson): reads the CURRENT byte straight out of the corpus .sav first and flips
    it, so a corpus that already has the flag set does not make surgery write 0 bytes
    and pass vacuously. Gold/Crystal only -- Gen 1 has no lucky-number system at all
    (GBF_LUCKY_NUMBER_SHOW_FLAG's Gen-1 gbf_off() is 0, do_counter's own gbf_off
    check refuses it outright)."""
    if info["gen"] != 2:
        tally.skip_case("counter (BACKLOG #88 D6, lucky)", "Gen 1 has no lucky-number system")
        return

    file_off = LUCKY_FILE_OFF[name]
    raw = sav.read_bytes()
    cur = raw[file_off]
    want_val = 0 if cur else 1

    edited = work / "counter.sav"
    rc, out, err = run_surgery(binary, sav, edited, [["counter", "lucky", str(want_val)]])
    if rc != 0:
        tally.record("counter (BACKLOG #88 D6, lucky)", False,
                    f"surgery refused: {err.strip()}")
        return

    identical = edited.read_bytes() == raw
    if identical:
        tally.record("counter (BACKLOG #88 D6, lucky)", False,
                    f"gate D6 class: surgery wrote 0 bytes (cur={cur} want={want_val}) "
                    f"-- counter lucky is a no-op")
        return

    addr = LUCKY_WRAM[name]
    want = f"{want_val:02x}"
    rc, rep, out, err = boot(python, rom, edited, work / "counter", vendor,
                             work / "counter.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:1"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    ok = (rc == 0) and svbk_ok and got == want
    detail = (f"verdict={rep.get('verdict')} svbk_ok={svbk_ok} {addr:#06x}={got!r} "
             f"want={want!r} cur={cur}")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("counter (BACKLOG #88 D6, lucky)", ok, detail)


def run_helditem_case(name, info, rom, sav, work, binary, python, vendor, tally,
                      party_count0):
    """BACKLOG #95 review gate case: proves --op helditem's write (gb_set_held_item)
    reached the BOOTED game, not just the .sav bytes on disk -- the review's own ask,
    since nothing upstream of this file exercised a Gen-2 setter against a live boot.
    Gold and Crystal only: HELDITEM_WRAM has no Red/Yellow entries (party slot 0's
    record offset 0x01 is current HP in Gen 1, not an item -- gb_set_held_item refuses
    it outright, tests/host_gbsurgery_tool.c's do_helditem already proves that refusal
    on the host; nothing left to boot for those two games)."""
    if info["gen"] == 1:
        tally.skip_case("held item (BACKLOG #95 review gate)",
                        "Gen 1 has no held-item field; gb_set_held_item refuses it")
        return
    if party_count0 < 1:
        tally.skip_case("held item (BACKLOG #95 review gate)", "empty party")
        return
    edited = work / "helditem.sav"
    rc, out, err = run_surgery(binary, sav, edited,
                               [["helditem", "party", "0", str(HELDITEM_VALUE)]])
    if rc != 0:
        tally.record("held item (BACKLOG #95 review gate)", False,
                    f"surgery refused: {err.strip()}")
        return

    addr = HELDITEM_WRAM[name]
    want = f"{HELDITEM_VALUE:02x}"
    rc, rep, out, err = boot(python, rom, edited, work / "helditem", vendor,
                             work / "helditem.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:1"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    ok = (rc == 0) and svbk_ok and got == want
    detail = (f"verdict={rep.get('verdict')} svbk={mem.get('svbk')} svbk_ok={svbk_ok} "
             f"{addr:#06x}={got!r} want={want!r}")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("held item (BACKLOG #95 review gate)", ok, detail)


def run_caught_case(name, info, rom, sav, work, binary, python, vendor, tally,
                    party_count0):
    """gbmon lane, BACKLOG #95 review C11's own binding test: proves --op caught's write
    (gb_set_caught, gated by gb_session_is_crystal -- the SAME function
    source/pdna_gen12.c's gb_mark_caught calls, so this case cannot pass by exercising
    a decision the live editor does not actually make) reaches the BOOTED game on
    Crystal, and is REFUSED outright by the surgery step itself on Gold -- proving the
    C11 fix's whole point (a Gen-2 target no longer gets a synthetic capture record
    just because it is Gen 2; it has to actually BE Crystal). Gen 1 has no capture
    record at all (gb_set_caught's own gen == GB_GEN2 check), so this case is Gen-2
    only, same posture as held item and box names."""
    if info["gen"] == 1:
        tally.skip_case("caught (BACKLOG #95 review gate, C11)",
                        "Gen 1 has no capture record; gb_set_caught refuses it")
        return
    if party_count0 < 1:
        tally.skip_case("caught (BACKLOG #95 review gate, C11)", "empty party")
        return

    packed = f"{CAUGHT_TIME}:{CAUGHT_LEVEL}:{CAUGHT_LOC}:{CAUGHT_GENDER}"
    edited = work / "caught.sav"
    rc, out, err = run_surgery(binary, sav, edited, [["caught", "party", "0", packed]])

    if name == "gold":
        # ---- the refusal case: no boot, the surgery step itself must say no ----
        ok = rc != 0
        detail = f"surgery rc={rc} (expected non-zero: has_caught gated false on Gold)"
        if not ok:
            detail += f" | stderr: {stderr_tail(err)}"
        tally.record("caught (BACKLOG #95 review gate, C11)", ok, detail)
        return

    # ---- Crystal: the surgery must succeed and the booted game's own WRAM byte must
    # read back exactly (time<<6)|level, per caught_data.asm's SetBoxmonOrEggmonCaughtData ----
    if rc != 0:
        tally.record("caught (BACKLOG #95 review gate, C11)", False,
                    f"surgery refused on Crystal (expected accept): {err.strip()}")
        return

    addr = CAUGHT_WRAM[name]
    want = f"{CAUGHT_WANT_BYTE:02x}"
    rc, rep, out, err = boot(python, rom, edited, work / "caught", vendor,
                             work / "caught.json",
                             extra_args=["--expect", "accept",
                                         "--read-mem", f"{addr:#06x}:1"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    got = mem.get(f"{addr:#06x}")
    ok = (rc == 0) and svbk_ok and got == want
    detail = (f"verdict={rep.get('verdict')} svbk_ok={svbk_ok} "
             f"{addr:#06x}={got!r} want={want!r} (wPartyMon1CaughtLevel)")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("caught (BACKLOG #95 review gate, C11)", ok, detail)


CREATE_DEX, CREATE_LEVEL, CREATE_NAME = 1, 5, "BULBASAUR"
# dex 1 = Bulbasaur, a base form -- rom_gblearn_min_level() computes 5 for it in
# BOTH gens (tests/host_romgblearn_test.c pins this exact value against real
# ROM data). CREATE_LEVEL is the EXPECTED result now, not a --op argument any
# more (BACKLOG #50 UX-parity, Guy 2026-09-07: the level PICKER this constant
# used to feed a chosen value into is gone from gb_create_hook, replaced by
# that same computation -- host_gbsurgery_tool.c's own --op create dropped its
# LEVEL argument to match, so it keeps mirroring gb_create_hook rather than
# silently drifting from it).


def create_case(python, vendor, work, rom, gen, sav, orig, sections, party_count0,
                base_name, binary, tally):
    """BACKLOG #50, item 4: create a mon via gb_new_mon (host_gbsurgery_tool.c's new
    --op create -- the SAME rom_gbbase_gen1/2 + rom_gblearn facts and the SAME
    gb_new_mon() call pdna_gen12.c's gb_create_hook makes) and boot the result.

    Box only: gbs_insert() itself refuses the party pseudo-box (gb_session.h; see
    gb_create_hook's own comment, source/pdna_gen12.c) -- Gen 2 then gbs_move()s the
    new box slot into the party (freeing a slot first, EXACTLY case 6 above's own
    "every corpus party is 6/6" pattern) so the retail game's OWN party screen shows
    its name+level; gb_roundtrip.py has no PC-box screen reader, only a party one.

    Gen 1 cannot take that second step: gbs_move()'s own box->party conversion
    refuses Gen 1 UNCONDITIONALLY (needs a Gen-1 base-stat table gbs_move has no
    parameter for -- the exact wall case 6 above already demonstrates and asserts
    as CORRECT, not a bug). So a Gen-1 create is asserted structurally instead --
    verdict=accept, party UNCHANGED, the SRAM-diff/length gates -- proving the
    created box record boots cleanly and is ACCEPTED by the retail game rather than
    read off a PC-box screen this harness cannot reach. Recorded as a known gap, not
    silently narrowed: the case label says "(Gen 1, box only, no on-screen content
    check)" so a passing run never reads as "the name was confirmed on screen"."""
    dst = first_room_box(sections)
    if dst is None:
        tally.skip_case("create", "every storage box this --list saw is full")
        return
    dst_count = sections[f"box{dst}"]["count"]   # gbs_insert() appends here (verified
                                                  # against the standalone tool: a box
                                                  # with count=17 lands its new mon at
                                                  # slot 17)

    if gen == 1:
        ops = [["create", str(dst), str(CREATE_DEX)]]
        label = f"create dex {CREATE_DEX}@{CREATE_LEVEL} into box {dst} (Gen 1, box only, no on-screen content check)"
        extra_check = None
    else:
        last_idx = party_count0 - 1
        ops = [["delete", "party", str(last_idx)],
               ["create", str(dst), str(CREATE_DEX)],
               ["move", str(dst), str(dst_count), "party"]]
        label = f"create dex {CREATE_DEX}@{CREATE_LEVEL} -> party"
        extra_check = check_level_slot(last_idx, gen, CREATE_NAME, CREATE_LEVEL)

    create_sav = work / "create.sav"
    rc, out, err = run_surgery(binary, sav, create_sav, ops, rom=rom)
    if rc != 0:
        tally.record("create", False, f"surgery refused: {err.strip()}")
        return

    edited_case(python, vendor, work, rom, gen, create_sav, len(orig), "create",
               ["--expect-name", base_name, "--expect-party-count", str(party_count0)],
               tally, label, extra_check=extra_check)


def run_game(name, info, rom, sav, scratch, binary, python, vendor):
    tally = Tally(sav.name)
    work = scratch / name
    work.mkdir(parents=True, exist_ok=True)
    gen = info["gen"]
    orig = sav.read_bytes()

    list_out = subprocess.run([str(binary), "--in", str(sav), "--list"],
                              capture_output=True, text=True).stdout
    sections = parse_list(list_out)
    party = sections.get("party", {"count": 0, "slots": []})
    party_count0, party_slots0 = party["count"], party["slots"]

    # ---- 1. baseline ----
    rc, rep, out, err = boot(python, rom, sav, work / "baseline", vendor,
                             work / "baseline.json")
    base_name, base_pc = rep.get("player_name"), rep.get("party_count")
    base_ok = rep.get("verdict") == "accept" and base_pc == party_count0
    base_detail = f"verdict={rep.get('verdict')} name={base_name!r} party={base_pc}"
    if not base_ok:
        tail = stderr_tail(err)
        if tail:
            base_detail += " | stderr: " + tail
    if not tally.record("baseline", base_ok, base_detail):
        return tally   # nothing downstream can be trusted without a good baseline

    # ---- 2. pass-through ----
    pt_sav = work / "passthrough.sav"
    rc, out, err = run_surgery(binary, sav, pt_sav, [])
    identical = rc == 0 and pt_sav.exists() and pt_sav.read_bytes() == orig
    if identical:
        edited_case(python, vendor, work, rom, gen, pt_sav, len(orig), "passthrough",
                   ["--expect-name", base_name, "--expect-party-count", str(base_pc)],
                   tally, "pass-through")
    else:
        tally.record("pass-through", False,
                    f"surgery rc={rc} identical={identical}: {err.strip()}")

    # ---- 2b. BACKLOG #49 P0 — the G/S backup-rescue regression case ----
    if party_count0 >= 1:
        run_mirror_case(name, info, rom, sav, work, binary, python, vendor, tally, base_name)
    else:
        tally.skip_case("G/S backup rescue (mirror, BACKLOG #49 P0)", "empty party")

    # ---- 2c. BACKLOG #49 P0 — the field-write primitive, proven with money ----
    run_money_case(name, info, rom, sav, work, binary, python, vendor, tally)

    # ---- 2c1. BACKLOG #126c — statusflags/gender writes, proven the same shape ----
    run_statusflags_case(name, info, rom, sav, work, binary, python, vendor, tally)
    run_gender_case(name, info, rom, sav, work, binary, python, vendor, tally)

    # ---- 2c1b. BACKLOG #89 — the Hall of Fame: clear then a plain count set ----
    run_hofclear_case(name, info, rom, sav, work, binary, python, vendor, tally)
    run_hofcount_case(name, info, rom, sav, work, binary, python, vendor, tally)
    # BACKLOG #194 F3 — append then delete (chained: delete acts on append's own
    # edited save, "work / hofappend.sav", so ORDER MATTERS -- append must run first).
    run_hofappend_case(name, info, rom, sav, work, binary, python, vendor, tally)
    run_hofdelete_case(name, info, rom, sav, work, binary, python, vendor, tally)

    # ---- 2c2. BACKLOG #95 review gate case — the held-item Pokemon-setter write ----
    run_helditem_case(name, info, rom, sav, work, binary, python, vendor, tally,
                      party_count0)

    # ---- 2c3. gbmon lane, BACKLOG #95 review C11 — the caught/capture-record write,
    # gated on gb_session_is_crystal so this proves the LIVE EDITOR's own decision ----
    run_caught_case(name, info, rom, sav, work, binary, python, vendor, tally,
                    party_count0)

    # ---- 2d. BACKLOG #49 P1a — the trainer-card core, proven with badges + name ----
    run_trainer_case(name, info, rom, sav, work, binary, python, vendor, tally)

    # ---- 2e. BACKLOG #49 P2a — the bag/PC-item core, proven with a Potion (+ Ball) ----
    run_bag_case(name, info, rom, sav, work, binary, python, vendor, tally)

    # ---- 2f. U5 (BACKLOG #67) — the TM/HM count array, Gen 2 only ----
    if info["gen"] == 2:
        run_tmhm_case(name, info, rom, sav, work, binary, python, vendor, tally)

    # ---- lane gbdata dispatch (BACKLOG #85/#86/#90/#94) — steps 2g-2j, deliberately
    # left a gap after 2e/2f so u5's own TM/HM case (2f) can land ahead of this block
    # without renumbering either lane: if 2f already exists above this marker when the
    # two lanes merge, renumber the four cases below it to 2h-2k instead of editing
    # around u5's insertion point. P1a review D10. ----
    # ---- 2g. BACKLOG #85 — the day-care core, proven with a deposit ----
    run_daycare_case(name, info, rom, sav, work, binary, python, vendor, tally)

    # ---- 2h. BACKLOG #86 — the Gen-2 clock core (Gen 1 has no clock: no case) ----
    if gen == 2:
        run_clock_case(name, info, rom, sav, work, binary, python, vendor, tally)
    else:
        tally.skip_case("clock (BACKLOG #86)", "Gen 1 has no clock")

    # ---- 2i. BACKLOG #90 — the fly-destination bitfield core (run_fly_case itself
    # skips Gen 1 -- every real destination is already visited in the corpus, D6) ----
    run_fly_case(name, info, rom, sav, work, binary, python, vendor, tally)

    # ---- 2i2. BACKLOG #87 item 6 — the Pokedex owned/seen core, both directions ----
    run_dexset_case(name, info, rom, sav, work, binary, python, vendor, tally)

    # ---- 2i3. BACKLOG #87 D4 — the Unown-dex gate (wStatusFlags bit 1 /
    # wFirstUnownSeen) stays in sync with a direct dex-201 edit. Gen 1 has no Unown
    # dex entry -- run_unown_gate_case itself skips with an honest reason. ----
    run_unown_gate_case(name, info, rom, sav, work, binary, python, vendor, tally)

    # ---- 2j. BACKLOG #94 — the Gen-2 box-name core (Gen 1 has no box names: no case) ----
    if gen == 2:
        run_boxname_case(name, info, rom, sav, work, binary, python, vendor, tally)
    else:
        tally.skip_case("boxname (BACKLOG #94)", "Gen 1 has no box names")

    # ---- 2k. BACKLOG #88 — the Flags & counters screen's raw event-flag write
    # (gbfl_set) on all four games (run_flags_case itself picks the flag index and
    # WRAM anchor per game, Table FLAGS_CASE_INDEX/EVENT_FLAGS_WRAM above) ----
    run_flags_case(name, info, rom, sav, work, binary, python, vendor, tally)

    # ---- 2l. D6 (BACKLOG #88 review) — the COUNTERS tab's lucky-number show flag,
    # Gen 2 only (run_counter_case itself skips Gen 1) ----
    run_counter_case(name, info, rom, sav, work, binary, python, vendor, tally)

    if party_count0 < 2:
        tally.skip_case("nickname/level/delete/move",
                        f"party has only {party_count0} member(s), need >= 2")
        return tally

    slot0 = party_slots0[0]
    last_idx = party_count0 - 1
    last_nick = party_slots0[last_idx]["nick"]

    # ---- 3. nickname edit, party slot 0 ----
    nick_sav = work / "nick.sav"
    rc, out, err = run_surgery(binary, sav, nick_sav, [["nick", "party", "0", NEW_NICK]])
    if rc == 0:
        edited_case(python, vendor, work, rom, gen, nick_sav, len(orig), "nick",
                   ["--expect-party", NEW_NICK, "--expect-name", base_name],
                   tally, "nickname", extra_check=check_nick_slot(0, NEW_NICK))
    else:
        tally.record("nickname", False, f"surgery refused: {err.strip()}")

    # ---- 4. level edit, party slot 0 ----
    new_level = 50 if slot0["level"] != 50 else 40
    lvl_sav = work / "level.sav"
    rc, out, err = run_surgery(binary, sav, lvl_sav,
                               [["level", "party", "0", str(new_level)]])
    if rc == 0:
        # Levels ARE readable off the party screen (an earlier revision of this file
        # claimed otherwise -- see docs/GEN12-EDIT-DESIGN.md S4 for the correction); the
        # slot-anchored check below reads the new level back off the exact row this slot
        # draws and also re-confirms the untouched nickname still leads that row.
        edited_case(python, vendor, work, rom, gen, lvl_sav, len(orig), "level",
                   ["--expect-name", base_name],
                   tally, f"level {slot0['level']}->{new_level}",
                   extra_check=check_level_slot(0, gen, slot0["nick"], new_level))
    else:
        tally.record("level", False, f"surgery refused: {err.strip()}")

    # ---- 5. delete party's last slot ----
    del_sav = work / "delete.sav"
    rc, out, err = run_surgery(binary, sav, del_sav, [["delete", "party", str(last_idx)]])
    if rc == 0:
        edited_case(python, vendor, work, rom, gen, del_sav, len(orig), "delete",
                   ["--expect-party-count", str(party_count0 - 1),
                    "--expect-no-screen", last_nick],
                   tally, "delete party last")
    else:
        tally.record("delete", False, f"surgery refused: {err.strip()}")

    # ---- 6. move box 0 slot 0 -> party (free a slot first: every corpus party is 6/6) --
    box0_slots = sections.get("box0", {}).get("slots", [])
    case6 = "Gen-1 box0->party refused" if gen == 1 else "Gen-2 box0->party"
    if not box0_slots:
        # box 0 empty (or unreadable) means slot 0 isn't OCCUPIED, so gbs_move would
        # refuse with GBS_ERR_SLOT before ever reaching the check this case exists to
        # test (GBS_ERR_FULL / GBS_ERR_NEEDS_BASE) -- an untested corpus save with a
        # thin box 0 must SKIP this case, not crash (a bare sections["box0"]["slots"][0]
        # raised IndexError here on an empty box) and not silently pass it either.
        tally.skip_case(case6, "box 0 is empty (or unreadable) -- nothing to move")
    else:
        box0_nick = box0_slots[0]["nick"]
        b2p_ops = [["delete", "party", str(last_idx)], ["move", "0", "0", "party"]]
        b2p_sav = work / "box2party.sav"
        rc, out, err = run_surgery(binary, sav, b2p_sav, b2p_ops)
        if gen == 1:
            refused_right = (rc == 1 and not b2p_sav.exists()
                             and "needs base stats" in err.lower())
            tally.record(case6, refused_right,
                        f"rc={rc} wrote_file={b2p_sav.exists()} stderr={err.strip()!r}")
        else:
            if rc == 0:
                edited_case(python, vendor, work, rom, gen, b2p_sav, len(orig),
                           "box2party",
                           ["--expect-party-count", str(party_count0),
                            "--expect-party", box0_nick],
                           tally, case6)
            else:
                tally.record(case6, False, f"surgery refused: {err.strip()}")

    # ---- 7. move party's last slot -> the first storage box with room ----
    dst = first_room_box(sections)
    if dst is None:
        tally.skip_case("party->box", "every storage box this --list saw is full")
    else:
        p2b_sav = work / "party2box.sav"
        rc, out, err = run_surgery(binary, sav, p2b_sav,
                                   [["move", "party", str(last_idx), str(dst)]])
        if rc == 0:
            edited_case(python, vendor, work, rom, gen, p2b_sav, len(orig), "party2box",
                       ["--expect-party-count", str(party_count0 - 1),
                        "--expect-no-screen", last_nick],
                       tally, f"party last -> box {dst}")
        else:
            tally.record("party->box", False, f"surgery refused: {err.strip()}")

    # ---- 8. create a mon from scratch (BACKLOG #50 item 4) ----
    create_case(python, vendor, work, rom, gen, sav, orig, sections, party_count0,
               base_name, binary, tally)

    return tally


def run_trainer_case(name, info, rom, sav, work, binary, python, vendor, tally):
    """BACKLOG #49 P1a -- gb_trainer.h's gbt_read/gbt_write (source/gb_trainer.c), via
    the surgery tool's new `--op badges MASK` (Gen 1) / `--op badges2 JOHTO KANTO`
    (Gen 2, P1a review D8 -- a single shared mask made a Johto/Kanto swap invisible to
    this gate) / `--op name TEXT`. Neither is a raw byte poke: both go through a
    read-whole-record / change-one-field / write-whole-record cycle. This case does NOT
    itself prove every other trainer-card field (money, coins, mom's money, play time)
    came back unchanged -- it reads none of them back -- that proof is
    tests/host_gbtrainer_test.c's full-image byte-diff round trip (P1a review D9); what
    this case adds on top is that the edit survives a REAL BOOT: a wrong offset in the
    money/coins path could still corrupt an adjacent byte badly enough that the game
    refuses to load at all, which host_gbtrainer_test.c's in-memory checks cannot see.

    Badges has no on-screen decimal readout this driver scrapes reliably across all four
    games, so it is asserted purely off WRAM (--read-mem, docs/GEN12-PARITY-DESIGN.md
    §4.2's own table) -- gated on SVBK the same way run_money_case's read is. The player
    name is asserted BOTH ways (P1a review D2 -- WRAM was read but never actually
    compared before this fix): off WRAM (exact bytes -- "GATET" is 0x86 0x80 0x93 0x84
    0x93 in the GB charset, then 0x50 fill out to NAME_LEN) and off the continue
    screen's own PLAYER= label, which gb_roundtrip.py decodes with its OWN independent
    GB-text reader -- agreement between the two is evidence gb_trainer.c's
    gb_name_encode did not spell the name in a way the game's own font draws
    differently."""
    edited = work / "trainer.sav"
    badges_op = (["badges", hex(BADGES_MASK)] if info["gen"] == 1
                else ["badges2", hex(BADGES_MASK), hex(BADGES_MASK_KANTO)])
    rc, out, err = run_surgery(binary, sav, edited, [badges_op, ["name", TRAINER_NAME]])
    if rc != 0:
        tally.record("trainer card (badges+name, BACKLOG #49 P1a)", False,
                    f"surgery refused: {err.strip()}")
        return

    badges_addr = BADGES_WRAM[name]
    badges_len = 1 if info["gen"] == 1 else 2
    name_addr = NAME_WRAM[name]
    rc, rep, out, err = boot(python, rom, edited, work / "trainer", vendor,
                             work / "trainer.json",
                             extra_args=["--expect", "accept",
                                         "--expect-name", TRAINER_NAME,
                                         "--read-mem", f"{badges_addr:#06x}:{badges_len}",
                                         "--read-mem", f"{name_addr:#06x}:{NAME_LEN}"])
    mem = rep.get("mem") or {}
    svbk_ok = bool(mem.get("svbk_ok", True))
    want_badges = (f"{BADGES_MASK:02x}" if info["gen"] == 1
                  else f"{BADGES_MASK:02x}{BADGES_MASK_KANTO:02x}")
    got_badges = mem.get(f"{badges_addr:#06x}")
    badges_ok = svbk_ok and got_badges == want_badges
    name_screen_ok = rep.get("player_name") == TRAINER_NAME
    # "GATET" -> G=0x86 A=0x80 T=0x93 E=0x84 T=0x93, then 0x50-filled to NAME_LEN
    # (P1a review D2: this WRAM read used to be printed but never actually compared).
    want_name_wram = "8680938493" + "50" * (NAME_LEN - 5)
    got_name_wram = mem.get(f"{name_addr:#06x}")
    name_wram_ok = svbk_ok and got_name_wram == want_name_wram
    ok = (rc == 0) and badges_ok and name_screen_ok and name_wram_ok
    detail = (f"verdict={rep.get('verdict')} svbk={mem.get('svbk')} svbk_ok={svbk_ok} "
             f"badges@{badges_addr:#06x}={got_badges!r} want={want_badges!r} "
             f"player_name={rep.get('player_name')!r} want={TRAINER_NAME!r} "
             f"name_wram@{name_addr:#06x}={got_name_wram!r} want={want_name_wram!r}")
    if not ok:
        fails = [f.strip() for f in out.splitlines() if f.strip().startswith("FAIL:")]
        if fails:
            detail += " | " + "; ".join(fails)
        tail = stderr_tail(err)
        if tail:
            detail += " | stderr: " + tail
    tally.record("trainer card (badges+name, BACKLOG #49 P1a)", ok, detail)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--corpus", default=str(DEFAULT_CORPUS))
    ap.add_argument("--scratch", default=str(DEFAULT_SCRATCH))
    ap.add_argument("--mgba-vendor", default=str(DEFAULT_VENDOR))
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--only", choices=list(GAMES))
    ap.add_argument("--keep", action="store_true", help="don't delete the scratch dir")
    a = ap.parse_args(argv)

    scratch = Path(a.scratch)
    corpus = Path(a.corpus)
    guard_scratch_path(scratch, corpus)
    prepare_scratch(scratch)

    t0 = time.time()
    binary = build_tool(scratch)

    names = [a.only] if a.only else list(GAMES)
    total_ok = total_fail = total_skip = 0
    for name in names:
        info = GAMES[name]
        rom = Path(a.corpus) / info["rom"]
        sav = Path(a.corpus) / info["sav"]
        if not rom.exists() or not sav.exists():
            print(f"[skip] {name} -- {rom.name}/{sav.name} not found in {a.corpus}")
            total_skip += 1
            continue
        tally = run_game(name, info, rom, sav, scratch, binary, a.python, a.mgba_vendor)
        total_ok += tally.ok
        total_fail += tally.fail
        total_skip += tally.skip

    dt = time.time() - t0
    print(f"\n{total_ok} ok, {total_fail} FAIL, {total_skip} skip -- {dt:.1f}s wall")

    if not a.keep:
        # Safe without re-checking the marker: this is the SAME directory
        # guard_scratch_path()/prepare_scratch() already verified and stamped at the
        # top of this run.
        shutil.rmtree(scratch, ignore_errors=True)
    else:
        print(f"scratch kept at {scratch}")
    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
