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

THE TWO GATES, and why both run on every edited boot (docs/GEN12-EDIT-DESIGN.md trap 7):
  PRIMARY — a content assertion read off the screen the game itself drew (--expect-party,
    --expect-no-screen, --expect-party-count). This is the one that catches a save the
    game silently discarded: Crystal boots the stale backup and shows the OLD data with
    verdict=accept and the right trainer name, so anything less than "is MY edit visible"
    waves it through.
  SECONDARY — the SRAM diff gb_roundtrip.py already computes for every run (no --dump-save
    needed for the numbers; --dump-save is used here ADDITIONALLY for an independent,
    file-level cross-check). The signal: a Gen-2 load that behaves correctly never
    rewrites the PRIMARY copy at all (only the backup mirror, and only because two copies
    disagreed before the game ever ran) — so sram_changed_primary_bytes != 0 means the
    primary we handed the emulator (WITH our edit already baked in) was rejected and the
    backup got booted instead, even though the screen may look fine. Gen 1 carries no such
    split; a good Gen-1 load rewrites NO save byte at all (gb_roundtrip.py's own selftest
    asserts exactly that), so sram_changed_bytes must be 0 there. Either way this is
    SECONDARY, not primary: it is not a bound on "how big was our edit" (the diff is
    before-load vs after-load, and our edit already happened offline, before the emulator
    ever saw the file) — it is a check that the GAME did not react to it by falling back to
    a stale copy.
  Independently of both: every edited boot's --dump-save is diffed against the edited
  INPUT file (not the original), SRAM only (split_rtc_tail, reused from gb_roundtrip.py —
  an MBC3 RTC footer is mGBA's clock, never save data and never part of this comparison).
  A length change there is a hard failure on its own, regardless of what the two internal
  gates above say.

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


def run_surgery(binary: Path, in_path: Path, out_path: Path, op_groups):
    """op_groups: list of token-lists, one per --op (e.g. ["nick","party","0","GATEX"])."""
    cmd = [str(binary), "--in", str(in_path), "--out", str(out_path)]
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


def secondary_gate(rep: dict, gen: int):
    """THE secondary gate (see module docstring): did the game's own load-time SRAM
    rewrite behave like an ACCEPTED save, independent of what the screen showed?"""
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
        stderr_tail = [l for l in err.strip().splitlines() if l.strip()][-5:]
        if stderr_tail:
            detail += " | stderr: " + " / ".join(stderr_tail)
    return tally.record(label, ok, detail)


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
    if not tally.record("baseline", rep.get("verdict") == "accept" and base_pc == party_count0,
                        f"verdict={rep.get('verdict')} name={base_name!r} party={base_pc}"):
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
    box0_nick = sections.get("box0", {}).get("slots", [{}])[0].get("nick")
    b2p_ops = [["delete", "party", str(last_idx)], ["move", "0", "0", "party"]]
    b2p_sav = work / "box2party.sav"
    rc, out, err = run_surgery(binary, sav, b2p_sav, b2p_ops)
    if gen == 1:
        refused_right = (rc == 1 and not b2p_sav.exists()
                         and "needs base stats" in err.lower())
        tally.record("Gen-1 box0->party refused", refused_right,
                    f"rc={rc} wrote_file={b2p_sav.exists()} stderr={err.strip()!r}")
    else:
        if rc == 0:
            edited_case(python, vendor, work, rom, gen, b2p_sav, len(orig), "box2party",
                       ["--expect-party-count", str(party_count0),
                        "--expect-party", box0_nick],
                       tally, "Gen-2 box0->party")
        else:
            tally.record("Gen-2 box0->party", False, f"surgery refused: {err.strip()}")

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

    return tally


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
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True, exist_ok=True)

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
        shutil.rmtree(scratch, ignore_errors=True)
    else:
        print(f"scratch kept at {scratch}")
    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
