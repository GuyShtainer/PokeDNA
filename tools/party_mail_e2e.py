#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""party_mail_e2e.py -- BACKLOG #228, the emulator half of the party-mail proof.

tools/read_party_mail.c (already landed) is the HOST half: given a raw 128 KiB
Gen-3 save image, it parses it with the SAME gen3_save.c every other host tool
trusts and prints one party slot's mail byte (gen3_save.h's G3_PARTY_MAIL_OFF
0x55; G3_MAIL_NONE 0xFF means "no mail", 0x00 is a real mail-slot index --
BACKLOG #225's own citation, pokeemerald include/pokemon.h:219-232 +
include/constants/items.h:448). What was missing is the OTHER half: driving
PokeDNA's own UI in a real emulator session to make a REAL flash write, then
reading that write back WITHOUT going through mGBA's `core.memory.sram`
binding -- BACKLOG #228's own finding is that binding is a hardcoded 64 KiB
window (mgba/gba.py's static offset/size table) against this project's real
128 KiB Flash1M chip, so a straight `core.memory.sram` dump silently omits
SaveBlock1's own logical section 1 (party count/array near offset 0x234) on
a live boot -- confirmed live in that lane: the 64 KiB window held sections
3-13+0 valid, sections 1/2 blank.

THE ROUTE (found working by the b225 review, /tmp/b225-proof/readback2.py,
reused here rather than re-derived): attach an EMPTY mgba.vfs.VFile as the
save backend BEFORE core.reset() (`core.load_save(vf)`), let the app run and
write its own flash through the emulated Flash1M protocol exactly as it would
on hardware, then `vf.seek(0, 0); vf.read_all()` for the complete, real
128 KiB image the app itself produced -- no window, no bank-switch replay.
Byte-level mutation of `core.memory.u8[addr] = v` raises TypeError on this
mGBA Python binding (the s179-a4 design note that stopped a prior attempt);
this script never writes through that path at all, only reads via VFile.

THE UI RECIPE (BACKLOG #225's own real repro, tools/g3_shots.py's
run_b225_party_mail, reused verbatim -- not re-derived): on Guy's real
Emerald.sav (fused with `tools/fuse_sav.py`, no --gb), box 0 slot 0 is
Magnemite (species 81) and the party is already 6/6, so picking it up and
dropping it on the party overlay's seeded panel slot takes the SWAP branch
(party_place_held), which calls the SAME box_to_party()/em_set_party_flag(e,
true) choke point the ADD path does (BACKLOG #225's fix site,
source/gen3_edit.c). SEL (cursor mode -> MOVE), A (pick up cell 0), UP (grid
top -> top tabs, PARTY, still carrying), A (party_strip_overlay PLACE mode),
A (swap onto the panel's seeded slot), UP UP (grid -> title -> top tabs),
RIGHT (PARTY -> SAVE), A (save confirm), A (yes -- the real flashsave.c
toggle-bit-polling write), a long settle (empirically far past BIG_SETTLE --
g3_shots.py's own comment: "nowhere near done at 180 frames").

Usage:
    /usr/local/bin/python3 tools/party_mail_e2e.py FUSED_EMERALD.gba \
        --slot 1 --out /tmp/party_mail_e2e --mgba-vendor /tmp/rec2mp4/vendor

Exit 0 and "PASS" if the readback's slot mail byte == 0xFF (G3_MAIL_NONE);
exit 1 and "FAIL" (printing the actual byte) otherwise -- MUTATION-provable
by pointing --rom at a build compiled from a scratch copy with BACKLOG #225's
one fix line (source/gen3_edit.c's `e->raw[G3_PARTY_MAIL_OFF] = G3_MAIL_NONE;`)
reverted, which must make this script report FAIL with byte 0x00.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

KEY = dict(A=0x1, B=0x2, SEL=0x4, START=0x8, RIGHT=0x10, LEFT=0x20,
           UP=0x40, DOWN=0x80, R=0x100, L=0x200)
HOLD = 3
BIG_SETTLE = 40

G3_PARTY_MAIL_OFF = 0x55
G3_MAIL_NONE = 0xFF


def _load_mgba(vendor: str | None):
    if vendor:
        sys.path.insert(0, vendor)
    import mgba.core, mgba.image, mgba.log, mgba.vfs  # noqa: E402
    mgba.log.silence()
    return mgba.core, mgba.image, mgba.vfs


def _tap(core, name: str, settle: int = BIG_SETTLE) -> None:
    core.set_keys(raw=KEY[name])
    for _ in range(HOLD):
        core.run_frame()
    core.set_keys(raw=0)
    for _ in range(settle):
        core.run_frame()


def drive_and_read_back(rom: Path, out_dir: Path, vendor: str | None) -> Path:
    """Boots `rom` with an EMPTY VFile-backed save, drives BACKLOG #225's own
    real box(0,0) -> party SWAP + SAVE sequence, then reads the resulting
    128 KiB flash image straight off the VFile (never core.memory.sram) and
    writes it to out_dir/flash.sav. Returns that path."""
    core_mod, image_mod, vfs = _load_mgba(vendor)
    core = core_mod.load_path(str(rom))
    if core is None:
        sys.exit(f"party_mail_e2e: mgba could not load {rom}")
    screen = image_mod.Image(*core.desired_video_dimensions())
    core.set_video_buffer(screen)
    vf = vfs.VFile.fromEmpty()
    core.load_save(vf)
    core.reset()

    def run(n: int) -> None:
        for _ in range(n):
            core.run_frame()

    run(180)   # let the box screen settle (same 180-frame Session convention
               # every runner in this tree uses before its first real input)
    out_dir.mkdir(parents=True, exist_ok=True)
    screen.to_pil().convert("RGB").save(out_dir / "00_boot.png")

    _tap(core, "SEL")                 # cursor mode NORMAL -> MOVE
    _tap(core, "A")                   # pick up cell 0 (Magnemite, species 81) -> carrying
    _tap(core, "UP")                  # off the grid top -> top tabs, PARTY (still carrying)
    _tap(core, "A")                   # PARTY tab -> party_strip_overlay, PLACE mode
    screen.to_pil().convert("RGB").save(out_dir / "01_place_overlay.png")

    _tap(core, "A")                   # swap onto the panel's seeded slot
    _tap(core, "UP")                  # grid -> title row
    _tap(core, "UP")                  # title -> top tabs
    _tap(core, "RIGHT")               # PARTY -> SAVE
    _tap(core, "A")                   # SAVE -> "Save changes?" confirm dialog
    _tap(core, "A", settle=180)       # yes -> "Saving.. / Writing flash save.."
    run(3000)                         # the real toggle-bit-polling write -- g3_shots.py's
                                       # own comment: nowhere near done at 180 frames
    screen.to_pil().convert("RGB").save(out_dir / "02_saved.png")
    # Readback happens HERE, right after the verified-write confirmation, and
    # deliberately BEFORE any further tap: an earlier version of this script
    # dismissed "SAVED" first and landed on an unrelated "TRANSFER NOT SAVED
    # / Your Pokemon is still in the Bank" screen (Guy's real Emerald.sav
    # carries other pending Bank-transfer state, unconnected to this
    # party-mail scenario) -- irrelevant to what this script proves (the
    # flash bytes the SAVE above already wrote), so no further tap is driven.
    vf.seek(0, 0)
    data = vf.read_all()
    flash_path = out_dir / "flash.sav"
    flash_path.write_bytes(bytes(data))
    print(f"[party_mail_e2e] wrote {flash_path} ({len(data)} bytes)")
    return flash_path


def build_reader(root: Path) -> Path:
    """Compiles tools/read_party_mail.c once (host cc, no devkitARM needed --
    the same posture every host tool in this tree already uses)."""
    src_dir = root / "source"
    out_bin = Path("/tmp/party_mail_e2e_reader")
    cmd = ["cc", "-std=c11", "-O2", "-I", str(src_dir),
           str(root / "tools" / "read_party_mail.c"),
           str(src_dir / "gen3_save.c"), str(src_dir / "gen3_mon.c"),
           str(src_dir / "data_tables.c"),
           "-o", str(out_bin)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"party_mail_e2e: read_party_mail.c build failed:\n{r.stderr}")
    return out_bin


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom", type=Path)
    ap.add_argument("--slot", type=int, default=1,
                     help="party slot 0..5 to read back (default 1, the panel's own "
                          "seeded SWAP target per BACKLOG #225's run_b225_party_mail)")
    ap.add_argument("--out", type=Path, default=Path("/tmp/party_mail_e2e"))
    ap.add_argument("--mgba-vendor", type=str, default="/tmp/rec2mp4/vendor")
    ap.add_argument("--root", type=Path, default=ROOT)
    a = ap.parse_args(argv)

    flash_path = drive_and_read_back(a.rom, a.out, a.mgba_vendor)
    reader = build_reader(a.root)
    r = subprocess.run([str(reader), str(flash_path), str(a.slot)],
                        capture_output=True, text=True)
    print(r.stdout.strip())
    if r.stderr.strip():
        print(r.stderr.strip(), file=sys.stderr)
    if r.returncode != 0:
        print("[party_mail_e2e] FAIL: read_party_mail.c could not read that slot "
              "(see stderr above)")
        return 1

    # "slot N: level=L mail=0xXX"
    mail_hex = r.stdout.strip().rsplit("mail=0x", 1)[-1][:2]
    mail = int(mail_hex, 16)
    if mail == G3_MAIL_NONE:
        print(f"[party_mail_e2e] PASS: slot {a.slot} mail byte = 0x{mail:02X} "
              f"(G3_MAIL_NONE)")
        return 0
    print(f"[party_mail_e2e] FAIL: slot {a.slot} mail byte = 0x{mail:02X}, "
          f"expected 0x{G3_MAIL_NONE:02X} (G3_MAIL_NONE)")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
