#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""vsd_spike.py -- BACKLOG #179 step A1 (the spike). THROWAWAY: answers one question
and dumps numbers, does not build toward A2+ (no image factory, no burst mode, no
Session/tap hook). Deleted or replaced once A3 lands the real harness.

WHAT THIS PROVES (docs/briefs/s179-design.md S4.3/4.4, S7.10)
---------------------------------------------------------------
source/vsd.c's vsd_attach()/vsd_xfer() publish a request in a 32-byte EWRAM mailbox
(source/vsd.h's VsdBox) and spin on `ack == seq`, bounded by REG_VCOUNT-wrap counts
(4 frames for attach, 16 for a transaction) rather than iteration counts. This script
drives a real pokedna-delta.gba/.elf pair in headless mGBA one frame at a time, plays
the host side of the protocol by hand (no tools/vsd.py yet -- that is step A3), and
answers:

  1. does the attach handshake complete (active_flashcart flips NO_FLASHCART ->
     EZ_FLASH_OMEGA) within its 4-frame bound, and how many run_frame() calls did it
     actually take?
  2. does one READ transaction round-trip: does the GBA-side buffer end up holding
     exactly the bytes the host wrote, does the mailbox's `op` field flip back to
     VSD_OP_NONE (vsd.c's own proof-of-return signal -- see vsd_xfer()'s comment),
     and how many frames did that take?
  3. MUTATION (S7.10): run the SAME two questions against a build where s_spin_count's
     `volatile` qualifier has been stripped (a scratch copy, never this tree) and
     report whether the loop still returns.

Symbol addresses (s_vsd, active_flashcart) are resolved fresh via `nm` on the ELF
passed on the command line -- NOT hardcoded -- so a rebuild never goes stale. This is
explicitly the THROWAWAY shortcut docs/briefs/s179-design.md S4.1 reserves for step A3
("the harness finds &s_vsd without an ELF or a symbol table... by the project's own
established convention"): that convention is not needed to answer THIS lane's question.
"""
from __future__ import annotations

import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# ---- VsdBox field offsets (source/vsd.h) -----------------------------------------
OFF_MAGIC, OFF_SEQ, OFF_OP, OFF_SECTOR, OFF_COUNT, OFF_ADDR, OFF_STATUS, OFF_ACK = (
    0, 4, 8, 12, 16, 20, 24, 28,
)
VSD_MAGIC = 0x31445356
VSD_OP_NONE, VSD_OP_READ, VSD_OP_WRITE = 0, 1, 2
VSD_ST_BUSY, VSD_ST_OK, VSD_ST_ERR = 0, 1, 2
ACTIVE_FLASHCART_NONE, ACTIVE_FLASHCART_EVERDRIVE, ACTIVE_FLASHCART_EZFO = 0, 1, 2

ATTACH_FRAME_BUDGET = 4 + 4     # the C side's own bound, plus slack for boot frames
                                 # spent before vsd_attach() is even reached
XFER_FRAME_BUDGET = 16 + 4
BOOT_FRAME_CAP = 400            # hard cap on "never observed the request at all"


def nm_addr(elf: Path, symbol: str) -> int:
    """The exact runtime address of a defined symbol in `elf`, via `nm`. Raises if the
    symbol is absent (a stale/stripped build is a setup error, not a silent 0)."""
    out = subprocess.run(
        ["arm-none-eabi-nm", str(elf)], capture_output=True, text=True, check=True
    ).stdout
    pat = re.compile(r"^([0-9a-fA-F]+)\s+\S\s+" + re.escape(symbol) + r"$", re.MULTILINE)
    m = pat.search(out)
    if not m:
        sys.exit(f"vsd_spike: symbol {symbol!r} not found in {elf} (stale build?)")
    return int(m.group(1), 16)


def load_mgba():
    vendor = ROOT.parent / "rec2mp4" / "vendor"
    if not vendor.is_dir():
        sys.exit(f"vendor path missing: {vendor}")
    sys.path.insert(0, str(vendor))
    import mgba.core, mgba.log  # noqa: E402

    mgba.log.silence()
    return mgba.core


def run_spike(gba_path: Path, elf_path: Path, *, pattern_seed: int = 0xA5) -> dict:
    """Boots `gba_path`, plays the host side of the VSD protocol by hand, and returns
    a dict of the measurements the report needs. Never raises on a "never observed the
    request" condition -- that IS a possible, reportable outcome (S7.10's own point),
    encoded as frames=None."""
    core_mod = load_mgba()
    mailbox = nm_addr(elf_path, "s_vsd")
    active_flashcart_addr = nm_addr(elf_path, "active_flashcart")
    spin_count_addr = nm_addr(elf_path, "s_spin_count")

    core = core_mod.load_path(str(gba_path))
    if core is None:
        sys.exit(f"vsd_spike: mgba could not load {gba_path}")
    import mgba.image  # noqa: E402  (deferred: needs sys.path already patched)

    screen = mgba.image.Image(*core.desired_video_dimensions())
    core.set_video_buffer(screen)
    core.reset()

    u32 = core.memory.u32
    u8 = core.memory.u8

    result = {
        "attach_frames": None,
        "attach_ok": False,
        "xfer_frames": None,
        "xfer_ok": False,
        "buffer_matches": False,
        "spin_count_nonzero": False,
        "unaligned_bail": False,
    }

    served_attach = False
    served_xfer = False
    last_seq_acked = 0
    frame = 0
    attach_serve_frame = None
    xfer_serve_frame = None
    pattern = bytes((pattern_seed + i) & 0xFF for i in range(512))

    while frame < BOOT_FRAME_CAP:
        core.run_frame()
        frame += 1

        magic = u32.raw_read(mailbox + OFF_MAGIC)
        if magic != VSD_MAGIC:
            continue  # vsd_attach() has not published the handshake yet this frame

        seq = u32.raw_read(mailbox + OFF_SEQ)
        op = u32.raw_read(mailbox + OFF_OP)

        if not served_attach and seq != last_seq_acked:
            # The attach handshake: op is VSD_OP_NONE by construction (S4.3), ack
            # trails seq by definition here -- just ring the doorbell back.
            u32.raw_write(mailbox + OFF_ACK, seq)
            last_seq_acked = seq
            served_attach = True
            attach_serve_frame = frame
            continue

        if served_attach and not served_xfer and op == VSD_OP_READ and seq != last_seq_acked:
            addr = u32.raw_read(mailbox + OFF_ADDR)
            if 0x08000000 <= addr <= 0x0DFFFFFF:
                sys.exit(f"vsd_spike: ROM-sourced write target 0x{addr:08x} -- refusing "
                          "(S7.4's own hard-fail, even though this is a read)")
            if addr % 4 != 0:
                result["unaligned_bail"] = True
            for i, b in enumerate(pattern):
                u8.raw_write(addr + i, b)
            u32.raw_write(mailbox + OFF_STATUS, VSD_ST_OK)
            u32.raw_write(mailbox + OFF_ACK, seq)
            last_seq_acked = seq
            served_xfer = True
            xfer_serve_frame = frame
            xfer_addr = addr
            continue

        if served_xfer:
            break

    # ---- measurement 1: the attach handshake -------------------------------------
    if attach_serve_frame is not None:
        for f in range(attach_serve_frame, attach_serve_frame + ATTACH_FRAME_BUDGET):
            core.run_frame()
            frame += 1
            if u8.raw_read(active_flashcart_addr) == ACTIVE_FLASHCART_EZFO:
                result["attach_ok"] = True
                result["attach_frames"] = frame - attach_serve_frame
                break

    # ---- measurement 2: the read transaction, via vsd_xfer()'s own op==NONE tell -
    if xfer_serve_frame is not None:
        for f in range(xfer_serve_frame, xfer_serve_frame + XFER_FRAME_BUDGET):
            core.run_frame()
            frame += 1
            if u32.raw_read(mailbox + OFF_OP) == VSD_OP_NONE:
                result["xfer_ok"] = True
                result["xfer_frames"] = frame - xfer_serve_frame
                break
        got = bytes(u8.raw_read(xfer_addr + i) for i in range(512))
        result["buffer_matches"] = (got == pattern)

    result["spin_count_nonzero"] = u32.raw_read(spin_count_addr) != 0
    result["attach_serve_frame"] = attach_serve_frame
    result["xfer_serve_frame"] = xfer_serve_frame
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("gba", type=Path)
    ap.add_argument("elf", type=Path)
    ap.add_argument("--label", default="run")
    args = ap.parse_args()

    r = run_spike(args.gba, args.elf)
    print(f"=== vsd_spike [{args.label}] ===")
    print(f"  attach: served_at_frame={r['attach_serve_frame']} "
          f"ok={r['attach_ok']} frames_to_return={r['attach_frames']}")
    print(f"  xfer:   served_at_frame={r['xfer_serve_frame']} "
          f"ok={r['xfer_ok']} frames_to_return={r['xfer_frames']} "
          f"buffer_matches={r['buffer_matches']} unaligned={r['unaligned_bail']}")
    print(f"  spin_count_nonzero={r['spin_count_nonzero']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
