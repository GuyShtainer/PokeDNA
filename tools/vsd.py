#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""vsd.py -- the harness-hosted virtual SD server, BACKLOG #179 Phase A step A3.

Serves source/vsd.c's 32-byte EWRAM mailbox (VsdBox) out of a flat, sector-addressed
FAT16 image built by tools/vsd_img.c, one transaction per emulated frame
(docs/briefs/s179-design.md S4.1/S4.4). This module has no `main()` of its own -- it
is imported by tools/gb_shots.py's Session (the "single funnel for every frame in
both scripts", S4.5) and driven from there. `python3 tools/vsd.py IMG.img` is a
standalone smoke-test CLI for the image half only (does not touch mGBA).

LOCATING THE MAILBOX WITHOUT AN ELF (S4.1)
-------------------------------------------
source/vsd.c defines a `const volatile` locator record, g_pdna_vsd, exactly like
source/fused_gb.c's g_pdna_gbd / source/fused_rom.c's g_pdna_fuse: an 8-byte ASCII
magic ("PDNAVSD1") immediately followed by two little-endian u32s -- the mailbox's
RUNTIME EWRAM address (a compile-time constant: `&s_vsd`) and sizeof(VsdBox). Scanning
the raw .gba bytes for that magic and reading the 16 bytes after it gives the exact
address to poke through mgba's core.memory, with no `nm`/ELF involved (tools/fuse_gb.py
uses the identical convention for its own directory pointer).

PROTOCOL (mirrors source/vsd.c's vsd_attach()/vsd_xfer() exactly -- read those first)
--------------------------------------------------------------------------------------
1. The GBA side writes magic/op/sector/count/addr/status via vsd_attach() or
   vsd_xfer(), THEN publishes the doorbell (`seq`) LAST -- the host never observes a
   half-formed request.
2. service() is called once per emulated frame, AFTER core.run_frame() (the CPU is
   stopped for the whole call -- S4.4 says this is exactly why no barriers/volatile
   discipline is needed on the host side, and exactly why this test *cannot* catch a
   real race -- S7.2, admitted rather than claimed as coverage).
3. If magic is right and seq changed since the last serve: op==VSD_OP_NONE (a value
   vsd_xfer() can never publish with a NEW seq -- it early-returns before touching the
   mailbox on any other op, see vsd.c) means this is the ATTACH handshake -- just ring
   the doorbell back. Otherwise it is a real transaction: READ copies image bytes into
   GBA memory at `addr`; WRITE copies GBA memory at `addr` into the image. Both use
   core.memory.u32.raw_write/raw_read in 4-byte chunks (raw_write, never the buggy
   __setitem__ -- design S1's own "binding bug to know"), falling back to per-byte u8
   access for the trailing <4 bytes or when `addr` itself is not 4-aligned (also what
   the S7.3 "unaligned transfer" counter is watching for).

S7.4 -- ROM-SOURCED WRITES ARE A HARD HARNESS ERROR
-----------------------------------------------------
Any WRITE whose `addr` (the caller's SOURCE buffer) falls inside 0x08000000..
0x0DFFFFFF raises VsdRomSourceError immediately, rather than being served. On a real
Omega this is the documented rom-load-lab bug class: f_write from a ROM-resident
buffer silently writes the BOOTLOADER to the card. No emulator has ever been able to
see it; this server is the first vehicle that can.

FAILURE INJECTION (S4.7) -- tests/hostfat/ramdisk.h's own knob set, one layer further
out, so they are drivable through the app's REAL screens for the first time:
  --vsd-protect          every WRITE fails (a write-protected volume)
  --vsd-fail-write-in N  the Nth served WRITE (counting from now) fails, then heals
  --vsd-fail-at N        after N successful writes, ONE write fails, then heals
  --vsd-lie-after N      after N successful writes, every WRITE reports OK and keeps
                          nothing (the card that ACKs and stores nothing)
  --vsd-fail-read-at N   after N successful reads, ONE read fails, then heals
"""
from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from dataclasses import fields as dc_fields
from pathlib import Path

# ---- VsdBox field offsets (source/vsd.h) -----------------------------------------
OFF_MAGIC, OFF_SEQ, OFF_OP, OFF_SECTOR, OFF_COUNT, OFF_ADDR, OFF_STATUS, OFF_ACK = (
    0, 4, 8, 12, 16, 20, 24, 28,
)
VSD_MAGIC = 0x31445356
VSD_OP_NONE, VSD_OP_READ, VSD_OP_WRITE = 0, 1, 2
VSD_ST_BUSY, VSD_ST_OK, VSD_ST_ERR = 0, 1, 2

VSD_LOCATOR_MAGIC = b"PDNAVSD1"
ROM_LO, ROM_HI = 0x08000000, 0x0DFFFFFF   # S7.4's own range
SECTOR = 512


class VsdError(Exception):
    """Any harness-side protocol/config error -- never silently swallowed (golden
    rule 3: fail fast and loudly)."""


class VsdRomSourceError(VsdError):
    """S7.4: a disk_write whose source address is inside ROM. On real hardware this
    writes the bootloader to the card; the server refuses to simulate it."""


def find_mailbox(rom_bytes: bytes) -> tuple[int, int]:
    """Scans `rom_bytes` (a .gba image) for source/vsd.c's g_pdna_vsd locator record
    and returns (mailbox_addr, mailbox_size). Raises VsdError if it is missing or
    ambiguous (S4.1's own convention -- tools/fuse_gb.py's locate_record() disambiguates
    a multi-hit case with a proximity heuristic; this record is 16 bytes in a tiny
    dependency-free .rodata entry and has never collided in practice, so a second hit
    is treated as a hard setup error rather than guessed at)."""
    hits = []
    start = 0
    while True:
        i = rom_bytes.find(VSD_LOCATOR_MAGIC, start)
        if i < 0:
            break
        hits.append(i)
        start = i + 1
    if not hits:
        raise VsdError(
            "no PDNAVSD1 locator record found -- this build predates BACKLOG #179 "
            "or was not built with PDNA_DELTA")
    if len(hits) > 1:
        raise VsdError(
            f"{len(hits)} PDNAVSD1 occurrences at "
            f"{', '.join(hex(h) for h in hits)} -- ambiguous, refusing to guess")
    off = hits[0]
    if off % 4:
        raise VsdError(f"PDNAVSD1 locator record at 0x{off:X} is not 4-byte aligned")
    addr, size = struct.unpack_from("<II", rom_bytes, off + 8)
    # BACKLOG #179 A3 review D12: the locator record carries sizeof(VsdBox) precisely
    # so a scanner can sanity-check the hit (vsd_img.h/source/vsd.h's own comment) --
    # nothing was actually reading it. A wrong size means either a stale build (an
    # older VsdBox shape) or a false-positive magic hit; serving through either would
    # read/write the wrong mailbox fields silently.
    if size != 32:
        raise VsdError(f"PDNAVSD1 locator record at 0x{off:X} claims size={size}, "
                        f"expected 32 (sizeof(VsdBox)) -- stale build or a false hit")
    return addr, size


@dataclass
class VsdImage:
    """A flat, sector-addressed FAT16 volume file (no partition table -- tools/vsd_img.c
    builds it with FM_SFD, matching disk_ioctl's GET_SECTOR_SIZE=512). Held entirely in
    memory (`bytearray`) and flushed back to disk on demand; a chain never has more than
    a few MiB of image, so this is simpler and faster than seeking a file handle per
    transaction and matches design S4.6's "images live under /tmp/pokedna-vsd/" scale.

    Implements tests/hostfat/ramdisk.h's failure-injection knob set VERBATIM (S4.7) so
    the same fault vocabulary the host tests already use is drivable through mGBA.
    """

    path: Path
    data: bytearray
    sectors: int

    # failure injection knobs (all off by default)
    protect: bool = False
    fail_write_in: int = 0        # >0: fail that many writes from now, then heal
    fail_at: int = -1             # >=0: let that many writes land, fail exactly one
    lie_after: int = -1           # >=0: let that many land, then lie forever
    lying: bool = False
    fail_read_at: int = -1        # >=0: let that many reads land, fail exactly one

    # counters (mirror ramdisk.c's rd_* globals)
    writes_served: int = 0
    reads_served: int = 0
    write_fails: int = 0
    read_fails: int = 0
    lied_sectors: int = 0

    @classmethod
    def load(cls, path: Path) -> "VsdImage":
        raw = Path(path).read_bytes()
        if len(raw) % SECTOR != 0:
            raise VsdError(f"{path}: not sector-aligned ({len(raw)} bytes)")
        return cls(path=Path(path), data=bytearray(raw), sectors=len(raw) // SECTOR)

    def flush(self, out_path: "Path | None" = None) -> None:
        Path(out_path or self.path).write_bytes(bytes(self.data))

    def read(self, sector: int, count: int) -> "bytes | None":
        """Returns the sector data, or None if the read-fail knob fired (mirrors
        ramdisk.c's disk_read returning RES_ERROR -- the caller maps None to
        VSD_ST_ERR)."""
        if sector + count > self.sectors:
            raise VsdError(f"read out of range: sector={sector} count={count} "
                            f"sectors={self.sectors}")
        if self.fail_read_at >= 0:
            if self.fail_read_at == 0:
                self.fail_read_at = -1
                self.read_fails += 1
                return None
            self.fail_read_at -= 1
        self.reads_served += 1
        off = sector * SECTOR
        return bytes(self.data[off:off + count * SECTOR])

    def write(self, sector: int, payload: bytes) -> bool:
        """Returns False if the write was refused/lost (mirrors ramdisk.c's disk_write
        returning RES_ERROR, or RES_OK-but-discarded for the lying knob -- the CALLER
        still sees VSD_ST_OK for a lie, exactly like a real card that ACKs and keeps
        nothing; that asymmetry is the whole point of --vsd-lie-after, S4.7)."""
        if len(payload) % SECTOR:
            raise VsdError(f"write payload not sector-sized ({len(payload)} bytes)")
        count = len(payload) // SECTOR
        if sector + count > self.sectors:
            raise VsdError(f"write out of range: sector={sector} count={count} "
                            f"sectors={self.sectors}")
        if self.protect:
            self.write_fails += 1
            return False
        if self.fail_write_in > 0:
            self.fail_write_in -= 1
            self.write_fails += 1
            return False
        if self.fail_at >= 0:
            if self.fail_at == 0:
                self.fail_at = -1
                self.write_fails += 1
                return False
            self.fail_at -= 1
        if self.lie_after >= 0:
            if self.lie_after == 0:
                self.lying = True
            else:
                self.lie_after -= 1
        if self.lying:
            self.lied_sectors += count
            return True   # ACKs, keeps nothing
        off = sector * SECTOR
        self.data[off:off + len(payload)] = payload
        self.writes_served += count
        return True


class VsdServer:
    """Owns the mGBA `core`, the mailbox address, and a VsdImage. Call `service()`
    once per emulated frame, strictly AFTER `core.run_frame()` -- S4.4's whole
    no-barriers argument depends on the CPU being stopped for the duration of the
    call."""

    def __init__(self, core, mailbox_addr: int, image: VsdImage):
        self.core = core
        self.mailbox = mailbox_addr
        self.image = image
        self.last_seq_acked = 0
        self.attached = False
        self.unaligned_count = 0
        self.transactions_served = 0
        self.roundtrip_disk_write_calls = 0   # every served READ or WRITE, incl. attach

    def _read_bytes(self, addr: int, n: int) -> bytes:
        u32 = self.core.memory.u32
        u8 = self.core.memory.u8
        out = bytearray(n)
        i = 0
        if addr % 4 == 0:
            while i + 4 <= n:
                v = u32.raw_read(addr + i) & 0xFFFFFFFF
                out[i:i + 4] = v.to_bytes(4, "little")
                i += 4
        while i < n:
            out[i] = u8.raw_read(addr + i) & 0xFF
            i += 1
        return bytes(out)

    def _write_bytes(self, addr: int, payload: bytes) -> None:
        u32 = self.core.memory.u32
        u8 = self.core.memory.u8
        n = len(payload)
        i = 0
        if addr % 4 == 0:
            while i + 4 <= n:
                v = int.from_bytes(payload[i:i + 4], "little")
                u32.raw_write(addr + i, v)
                i += 4
        while i < n:
            u8.raw_write(addr + i, payload[i])
            i += 1

    def service(self) -> bool:
        """Returns True iff a transaction (attach handshake or real I/O) was served
        this call. Raises VsdRomSourceError on an S7.4 hit -- callers let this
        propagate; it is a harness bug class, not a recoverable condition."""
        u32 = self.core.memory.u32
        magic = u32.raw_read(self.mailbox + OFF_MAGIC) & 0xFFFFFFFF
        if magic != VSD_MAGIC:
            return False
        seq = u32.raw_read(self.mailbox + OFF_SEQ) & 0xFFFFFFFF
        if seq == self.last_seq_acked:
            return False

        op = u32.raw_read(self.mailbox + OFF_OP) & 0xFFFFFFFF

        if op == VSD_OP_NONE:
            # vsd_xfer() never publishes a NEW seq with op==NONE (it early-returns
            # before touching the mailbox for any op other than READ/WRITE) -- the
            # only way to reach here is vsd_attach()'s own initial handshake.
            u32.raw_write(self.mailbox + OFF_STATUS, VSD_ST_OK)
            u32.raw_write(self.mailbox + OFF_ACK, seq)
            self.last_seq_acked = seq
            self.attached = True
            return True

        sector = u32.raw_read(self.mailbox + OFF_SECTOR) & 0xFFFFFFFF
        count = u32.raw_read(self.mailbox + OFF_COUNT) & 0xFFFFFFFF
        addr = u32.raw_read(self.mailbox + OFF_ADDR) & 0xFFFFFFFF

        if addr % 4 != 0:
            self.unaligned_count += 1

        status = VSD_ST_ERR
        if op == VSD_OP_READ:
            data = self.image.read(sector, count)
            if data is not None:
                self._write_bytes(addr, data)
                status = VSD_ST_OK
        elif op == VSD_OP_WRITE:
            if ROM_LO <= addr <= ROM_HI:
                raise VsdRomSourceError(
                    f"disk_write source addr=0x{addr:08x} is inside ROM "
                    f"(0x{ROM_LO:08x}..0x{ROM_HI:08x}) -- S7.4's hard harness error: "
                    "on a real Omega this writes the BOOTLOADER to the card")
            payload = self._read_bytes(addr, count * SECTOR)
            status = VSD_ST_OK if self.image.write(sector, payload) else VSD_ST_ERR
        # else: unknown op -- status stays VSD_ST_ERR, still acked below so the GBA
        # side's spin loop is released rather than left to time out.

        u32.raw_write(self.mailbox + OFF_STATUS, status)
        u32.raw_write(self.mailbox + OFF_ACK, seq)
        self.last_seq_acked = seq
        self.transactions_served += 1
        self.roundtrip_disk_write_calls += 1
        return True


def attach_server(core, rom_bytes: bytes, img_path: Path, **knobs) -> VsdServer:
    """Convenience: locate the mailbox in `rom_bytes`, load `img_path`, build a
    VsdServer, and apply any failure-injection knob passed as a kwarg (e.g.
    `protect=True`, `fail_write_in=3`). Used by gb_shots.Session's --vsd hook."""
    addr, size = find_mailbox(rom_bytes)
    image = VsdImage.load(img_path)
    # BACKLOG #179 A3 review D12: a mistyped or renamed knob (e.g. a caller still
    # passing `fail_at_n=` after a rename) used to silently become a NEW, dead
    # instance attribute via plain setattr() -- the actual VsdImage field it meant to
    # set stayed at its default, and the caller's whole failure-injection scenario
    # never fired, with no error anywhere. Checked against the DATACLASS FIELD names
    # specifically (dataclasses.fields(), not hasattr()) -- hasattr() would also be
    # True for `read`/`write`/`flush` (bound methods), and setattr()ing one of those
    # would silently replace a method with whatever value a caller passed.
    field_names = {f.name for f in dc_fields(image)}
    for k, v in knobs.items():
        if v is None:
            continue
        if k not in field_names:
            raise VsdError(f"attach_server(): unknown VSD knob {k!r} -- not a "
                            f"VsdImage field")
        setattr(image, k, v)
    return VsdServer(core, addr, image)


def _cli(argv: "list[str] | None" = None) -> int:
    """Standalone smoke test of the image half only (no mGBA): loads an image, prints
    its sector count and the mailbox-locator search result against a .gba if given."""
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", type=Path, help="a tools/vsd_img.c mkimg .img file")
    ap.add_argument("--gba", type=Path, help="optionally also locate the mailbox in this .gba")
    args = ap.parse_args(argv)

    img = VsdImage.load(args.image)
    print(f"{args.image}: {img.sectors} sectors ({img.sectors * SECTOR} bytes)")
    if args.gba:
        addr, size = find_mailbox(args.gba.read_bytes())
        print(f"{args.gba}: mailbox at 0x{addr:08x}, size {size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(_cli())
