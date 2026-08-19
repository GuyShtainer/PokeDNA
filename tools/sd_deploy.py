#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Guy Shtainer
"""sd_deploy.py — copy a .gba to an EZ-Flash microSD and PROVE the card can load it.

USAGE
-----
    python3 tools/sd_deploy.py PokeDNA.gba /Volumes/EZFLASH
    python3 tools/sd_deploy.py PokeDNA.gba /Volumes/EZFLASH --as PokeDNA.gba --eject
    python3 tools/sd_deploy.py --check-only /Volumes/EZFLASH PokeDNA.gba   # measure, copy nothing
    python3 tools/sd_deploy.py PokeDNA.gba /Volumes/EZFLASH --check-only --as PokeDNA.gba
                                              # ^ measure AND compare against the build
    python3 tools/sd_deploy.py PokeDNA.gba /Volumes/EZFLASH --dry-run      # inspect + predict only
    python3 tools/sd_deploy.py PokeDNA.gba /Volumes/EZFLASH --image card.img   # test against a disk image

    Options:  --as NAME  --retries N  --retry-above N  --no-defrag  --balloon-max MiB
              --tidy  --eject  --json FILE  --dry-run  --check-only

    Exit codes:
        0  deployed, bytes verified, run table within the kernel's budget
        1  usage / environment / safety-guard error (nothing was written)
        2  BYTES BAD   — the card's copy differs from the source
        3  FRAGMENTED  — bytes are fine, the run table overruns the FPGA control words
        4  UNMEASURED  — the raw probe could not run (no root, or it errored). In a
                         deploy this means bytes were verified but fragmentation is
                         unknown; in --check-only it means PRESENCE ITSELF is unknown —
                         this is NOT evidence the file is absent. Do not re-copy on a 4.

WHY THIS EXISTS
---------------
A 12.5 MB PokeDNA.gba boots from NOR and refuses to boot from SD. That is not a
code bug and not a bad build: it is the *loader*, and it fails before a single
instruction of ours runs, so nothing on the cartridge can report it. Dragging
the file in Finder gives no signal at all — you find out by walking to the
console and watching "LOADING GAME" hang forever.

This tool turns that walk into a number, measured on the Mac, before you get up.

THE ARITHMETIC (every constant below is read out of the EZ-Flash kernel source at
 reference/omega-de-kernel/ — Apache-2.0, referenced for facts, not copied)
---------------------------------------------------------------------------
The Omega DE does not CPU-copy an SD-loaded ROM into PSRAM. It hands the FPGA a
list of cluster runs in a 1 KiB EWRAM buffer and lets the FPGA do the transfer:

    FAT_table_buffer[FAT_table_size/4]      ezkernelnew.c:38
    #define FAT_table_size 0x400            ez_define.h:8       (1024 bytes)

Check_game_RTS_FAT() (ezkernelnew.c:950) fills it by walking the file's FAT
chain. For the game ROM it is called with game_save_rts == 1 (ezkernelnew.c:2880),
which starts the write pointer at buffer offset 0:

    *FAT_table_P = 0; FAT_table_P++;                 ezkernelnew.c:993-996
    *FAT_table_P = ClustToSect(start); FAT_table_P++;      -> 8 bytes of header
    do {  ...
        if (getcluster != getcluster_old + 1) {      ezkernelnew.c:1002
            *FAT_table_P = cluster_num * csize; FAT_table_P++;
            *FAT_table_P = ClustToSect(getcluster); FAT_table_P++;   -> 8 bytes
        }
    } while (getcluster < lastest_cluster);          ezkernelnew.c:1012

There is NO bounds check anywhere in that loop. One 8-byte pair is emitted per
discontinuity, and the final end-of-chain transition is itself a discontinuity
(the EOC marker is never old+1), so a file occupying E extents ends the loop
having written exactly:

    fat_end = 8 + 8 * E     bytes

(The last pair is then backed over in place by the 0xFFFFFFFF/0x0 terminator —
 ezkernelnew.c:1014-1015 — so it does not add length.)

Meanwhile the FPGA's control words live at the top of that same buffer:

    FAT_table_buffer[0x1F0/4] = gamefilesize;      ezkernelnew.c:1798
    FAT_table_buffer[0x1F4/4] = DMA_COPY_MODE;     ezkernelnew.c:1799
    FAT_table_buffer[0x1F8/4] = csize;             ezkernelnew.c:1800
    FAT_table_buffer[0x1FC/4] = saveMODE|savesize; ezkernelnew.c:1801

so the run list's budget is bytes 0x000..0x1EF, and the predicate is

    margin = 0x1F0 - (8 + 8 * E)      safe iff margin >= 0
    =>  E <= 61 extents.  E == 62 overwrites the file size and the copy mode.

Then Send_FATbuffer() (Ezcard_OP.c:210) DMAs the buffer to the FPGA at 0x9E00000
and waits on it in two unbounded `while(1) { res = SD_Response(); ... }` loops
(Ezcard_OP.c:225-238) — no timeout, no CRC, no retry. A garbage file size means
that wait never ends. That is the hang, and it is a property of *where the file
landed on the card*, not of the file's contents. Which explains the whole
symptom set: intermittent, correlated with size (more clusters, more chances to
fragment), "fixed" by re-copying (re-copying defragments), and never, ever a
problem from NOR (the NOR path never builds this table).

Secondary budget, for completeness: the RTS table is written at offset 0x300
(FAT_table_RTS_offset, ez_define.h:10; GBApatch.c:952 calls with mode 3) into
the same 1 KiB buffer, so a .rts sidecar gets only (0x400-0x300)/8 - 1 = 31
extents before it runs off the end of the buffer entirely. This tool reports the
ROM budget; if you use RTS, keep the .rts file small and unfragmented too.

WHAT THIS TOOL DOES ABOUT IT
----------------------------
1. Guards the destination (must be an explicitly named FAT32/exFAT volume under
   /Volumes that is not the root filesystem — it will refuse a system disk).
2. Preflights the source (cloud-storage placeholders / partial hydration).
3. Reads the card's raw FAT and prints its free-space map *before* copying, so a
   card that physically cannot hold the file contiguously says so up front.
4. Copies, fsyncs, then UNMOUNTS and REMOUNTS — the only honest barrier on
   macOS. `sync` does not flush a removable device's own cache, and without the
   remount the verify would be reading the Mac's page cache, i.e. reading back
   the bytes it just wrote from RAM and calling that proof.
5. Verifies the bytes off the media (SHA-256 + CRC32, both sides).
6. Measures the on-card extents and computes the margin above.
7. If over budget — or if the pre-copy prediction already says it will be — it
   *acts*: delete + re-copy (which usually defragments), plus a bounded
   "balloon" pass that fills the small free holes first so the ROM lands in the
   largest contiguous free run. Every attempt is measured; the tool never
   declares a success it did not observe.

   Measured behaviour of the macOS msdos/exfat allocator, from the synthetic
   cards this was developed against: when a contiguous free run large enough
   for the whole file exists, it *usually* finds it — but not always. On a card
   whose front was pocked with 2000 x 128 KiB holes and which had one 16 MiB
   contiguous run further in, a plain copy of a 12 MB ROM landed in the holes
   and took 96 extents (a guaranteed hang); with the balloon pass it took 1.
   When no contiguous run is big enough, no allocation trick helps and the tool
   says so instead of writing filler for nothing.
8. If it still cannot get under budget it says so with the measured numbers and
   the specific remedy, rather than leaving you to find out at the console.

The two failure modes are reported separately and never conflated:
    BYTES BAD  -> re-copy / different card / hydrate the source. Do not blame load.
    FRAGMENTED -> defragment, free space, reformat with larger clusters, or NOR.

CREDIT
------
The raw FAT32/exFAT extent walk and the margin verdict are ported from
rom-load-lab/tools/sd-verify.sh (`cmd_fragcheck`), which root-caused this
failure; the constants there and here are the kernel's own. Extended here with
the free-space map, the first-fit prediction, and the remediation loop.

macOS NOTES (these matter and are not folklore)
-----------------------------------------------
* Spotlight and fseventsd write to a removable card behind your back. Their
  index files interleave with your ROM and are a real source of fragmentation.
  `--tidy` removes .Spotlight-V100/.fseventsd/.Trashes/._* and drops a
  `.metadata_never_index` marker at the card root so Spotlight leaves it alone.
* Finder copies leave `._NAME.gba` AppleDouble sidecars on FAT. Harmless to the
  cart, but they consume clusters and shuffle the allocator. Removed for the
  destination name automatically.
* Cluster size is set at format time and it directly sets how many clusters a
  12.5 MB ROM needs: at 32 KiB/cluster it is 383 clusters, at 4 KiB it is 3062 —
  ten times as many chances to fragment. exFAT on a 64 GB card typically formats
  at 128 KiB; FAT32 from macOS Disk Utility on a 32 GB card typically 32 KiB.
  If you keep losing this fight, reformatting FAT32 with the largest cluster
  size your card supports is the durable fix.
* Reading the raw partition needs root. This tool does the copy unprivileged and
  elevates only the measurement; if it cannot, it prints the exact command.
"""

import argparse
import array
import hashlib
import json
import os
import plistlib
import shlex
import shutil
import subprocess
import sys
import time
import zlib

# --------------------------------------------------------------------------
# Kernel constants. Sources cited in the module docstring; do not "tidy" these
# into something derived — they are transcribed facts.
# --------------------------------------------------------------------------
FAT_TABLE_SIZE = 0x400   # ez_define.h:8      whole buffer handed to the FPGA
CTRL_WORDS_OFF = 0x1F0   # ezkernelnew.c:1798 first FPGA control word
RTS_TABLE_OFF = 0x300    # ez_define.h:10     RTS run table start (informational)
RUN_HEADER = 8           # ezkernelnew.c:993-996   two words before the loop
RUN_PAIR = 8             # ezkernelnew.c:1004-1007 two words per discontinuity

# 8 + 8*E <= 0x1F0  ->  E <= 61
MAX_SAFE_RUNS = (CTRL_WORDS_OFF - RUN_HEADER) // RUN_PAIR      # 61
# margin >= 0x100 is "wide" in rom-load-lab's verdict table -> E <= 29
WIDE_SAFE_RUNS = (CTRL_WORDS_OFF - 0x100 - RUN_HEADER) // RUN_PAIR   # 29

CHUNK = 4 << 20
BALLOON_NAME = ".sd_deploy_balloon.tmp"

# --------------------------------------------------------------------------
# output
# --------------------------------------------------------------------------
_TTY = sys.stdout.isatty()
_R = "\033[31m" if _TTY else ""
_G = "\033[32m" if _TTY else ""
_Y = "\033[33m" if _TTY else ""
_B = "\033[1m" if _TTY else ""
_0 = "\033[0m" if _TTY else ""


def hdr(msg):
    print("\n%s== %s ==%s" % (_B, msg, _0))


def ok(msg):
    print("%s  OK  %s %s" % (_G, _0, msg))


def warn(msg):
    print("%s WARN %s %s" % (_Y, _0, msg))


def bad(msg):
    print("%s FAIL %s %s" % (_R, _0, msg))


def info(msg=""):
    print("       %s" % msg)


class Fatal(Exception):
    pass


def human(n):
    if n >= 1 << 20:
        return "%.2f MiB (%d bytes)" % (n / 1048576.0, n)
    if n >= 1024:
        return "%.1f KiB (%d bytes)" % (n / 1024.0, n)
    return "%d bytes" % n


# ==========================================================================
# Raw FAT32 / exFAT reader.  Ported from rom-load-lab/tools/sd-verify.sh
# (cmd_fragcheck), extended with a free-space map.
# ==========================================================================
class Raw:
    """Aligned reader over a raw partition device or a plain image file.

    /dev/diskNsM on macOS rejects unaligned reads on some releases, so every
    read is widened to a 4096-byte boundary and sliced back down. 4096 is a
    multiple of both 512- and 4096-byte device blocks.
    """

    ALIGN = 4096

    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDONLY)

    def close(self):
        try:
            os.close(self.fd)
        except OSError:
            pass

    def read(self, off, n):
        if n <= 0:
            return b""
        a = off & ~(self.ALIGN - 1)
        end = (off + n + self.ALIGN - 1) & ~(self.ALIGN - 1)
        out = bytearray()
        want = end - a
        pos = a
        while want > 0:
            blk = os.pread(self.fd, min(want, 8 << 20), pos)
            if not blk:
                break
            out += blk
            pos += len(blk)
            want -= len(blk)
        return bytes(out[off - a:off - a + n])


def _runs_from_clusters(clusters):
    """[c0, c1, ...] (in chain order) -> [(start, count), ...] extents."""
    runs = []
    start = None
    prev = None
    count = 0
    for c in clusters:
        if start is None:
            start, prev, count = c, c, 1
            continue
        if c == prev + 1:
            count += 1
            prev = c
        else:
            runs.append((start, count))
            start, prev, count = c, c, 1
    if start is not None:
        runs.append((start, count))
    return runs


def _free_runs_from_flags(is_free, first_cluster, n):
    """is_free(i) over clusters [first_cluster, first_cluster+n)."""
    runs = []
    start = None
    count = 0
    for i in range(n):
        c = first_cluster + i
        if is_free(i):
            if start is None:
                start, count = c, 1
            else:
                count += 1
        elif start is not None:
            runs.append((start, count))
            start, count = None, 0
    if start is not None:
        runs.append((start, count))
    return runs


def _summarise_free(free_runs, need_clusters):
    """Largest free run, cost of skipping past it, and a first-fit prediction."""
    total = sum(c for _, c in free_runs)
    largest = max(free_runs, key=lambda r: r[1]) if free_runs else (0, 0)
    before = 0
    for s, c in free_runs:
        if (s, c) == largest:
            break
        before += c
    # first-fit from the bottom of the free list: what a naive allocator gives.
    need = need_clusters
    predicted = 0
    for _s, c in free_runs:
        if need <= 0:
            break
        predicted += 1
        need -= c
    if need > 0:
        predicted = -1          # does not fit at all
    return {
        "free_run_count": len(free_runs),
        "free_clusters": total,
        "largest_free_run_start": largest[0],
        "largest_free_run_clusters": largest[1],
        "clusters_before_largest": before,
        "firstfit_predicted_extents": predicted,
    }


def probe_fat32(raw, bs, relpath):
    ss = int.from_bytes(bs[0x0B:0x0D], "little")
    spc = bs[0x0D]
    rsvd = int.from_bytes(bs[0x0E:0x10], "little")
    nfat = bs[0x10]
    fatsz = int.from_bytes(bs[0x24:0x28], "little")
    root = int.from_bytes(bs[0x2C:0x30], "little")
    tot16 = int.from_bytes(bs[0x13:0x15], "little")
    tot32 = int.from_bytes(bs[0x20:0x24], "little")
    if not ss or not spc or not fatsz:
        raise Fatal("FAT32 boot sector looks invalid (ss=%d spc=%d fatsz=%d)"
                    % (ss, spc, fatsz))
    clsz = ss * spc
    first_data = rsvd + nfat * fatsz
    totsec = tot32 or tot16
    n_clusters = max(0, (totsec - first_data) // spc)

    fat_bytes = raw.read(rsvd * ss, fatsz * ss)
    fat = array.array("I")
    fat.frombytes(fat_bytes[:(len(fat_bytes) // 4) * 4])
    if sys.byteorder != "little":
        fat.byteswap()

    def nxt(c):
        if c >= len(fat):
            return 0x0FFFFFFF
        return fat[c] & 0x0FFFFFFF

    def c2o(c):
        return (first_data + (c - 2) * spc) * ss

    def chain(start, want):
        out = []
        c = start
        while len(out) < want and 2 <= c < 0x0FFFFFF8:
            out.append(c)
            c = nxt(c)
        return out

    def dirdata(clust):
        data = bytearray()
        c = clust
        guard = 0
        while 2 <= c < 0x0FFFFFF8 and guard < 65536:
            data += raw.read(c2o(c), clsz)
            c = nxt(c)
            guard += 1
        return bytes(data)

    def walk(clust, parts):
        data = dirdata(clust)
        want = parts[0].lower()
        i = 0
        lfn = ""
        while i + 32 <= len(data):
            e = data[i:i + 32]
            if e[0] == 0x00:
                break
            if e[0] == 0xE5:
                i += 32
                lfn = ""
                continue
            if e[11] == 0x0F:
                part = (e[1:11] + e[14:26] + e[28:32]).decode("utf-16-le", "ignore")
                part = part.split("\x00")[0].split("￿")[0]
                lfn = part + lfn
                i += 32
                continue
            sfn = (e[0:8].decode("ascii", "ignore").strip() + "." +
                   e[8:11].decode("ascii", "ignore").strip()).strip(".")
            name = lfn or sfn
            lfn = ""
            first = (int.from_bytes(e[0x14:0x16], "little") << 16) | \
                    int.from_bytes(e[0x1A:0x1C], "little")
            size = int.from_bytes(e[0x1C:0x20], "little")
            isdir = bool(e[11] & 0x10)
            if name.lower() == want:
                if len(parts) == 1 and not isdir:
                    return ("file", first, size)
                if isdir and len(parts) > 1:
                    return walk(first, parts[1:])
            i += 32
        return None

    res = {"fs": "FAT32", "sector_size": ss, "sectors_per_cluster": spc,
           "cluster_size": clsz, "total_clusters": n_clusters}

    limit = min(n_clusters + 2, len(fat))
    free_runs = _free_runs_from_flags(lambda i: fat[2 + i] & 0x0FFFFFFF == 0,
                                      2, max(0, limit - 2))
    res["_free_runs"] = free_runs

    found = walk(root, relpath.strip("/").split("/")) if relpath else None
    if relpath and not found:
        res["found"] = False
        return res
    if found:
        _, first, size = found
        nclust = (size + clsz - 1) // clsz
        cl = chain(first, nclust)
        res.update({"found": True, "file_size": size, "clusters": nclust,
                    "clusters_seen": len(cl),
                    "extents": _runs_from_clusters(cl)})
    return res


def probe_exfat(raw, bs, relpath):
    fat_off = int.from_bytes(bs[0x50:0x54], "little")
    fat_len = int.from_bytes(bs[0x54:0x58], "little")
    heap_off = int.from_bytes(bs[0x58:0x5C], "little")
    n_clusters = int.from_bytes(bs[0x5C:0x60], "little")
    root = int.from_bytes(bs[0x60:0x64], "little")
    ss = 1 << bs[0x6C]
    spc = 1 << bs[0x6D]
    clsz = ss * spc

    def nxt(c):
        return int.from_bytes(raw.read(fat_off * ss + c * 4, 4), "little")

    def c2o(c):
        return (heap_off + (c - 2) * spc) * ss

    def chain(start, want, nofat):
        if nofat:
            return list(range(start, start + want))
        out = []
        c = start
        while len(out) < want and 2 <= c < 0xFFFFFFF6:
            out.append(c)
            c = nxt(c)
        return out

    def dirdata(clust):
        data = bytearray()
        c = clust
        guard = 0
        while 2 <= c < 0xFFFFFFF6 and guard < 65536:
            data += raw.read(c2o(c), clsz)
            c = nxt(c)
            guard += 1
        return bytes(data)

    def entries(data):
        """Yield (kind, dict) for 0x81 bitmap and 0x85 file sets."""
        i = 0
        while i + 32 <= len(data):
            e = data[i:i + 32]
            t = e[0]
            if t == 0x00:
                break
            if t == 0x81:
                yield ("bitmap", {
                    "flags": e[1],
                    "first": int.from_bytes(e[0x14:0x18], "little"),
                    "len": int.from_bytes(e[0x18:0x20], "little")})
                i += 32
                continue
            if t == 0x85:
                secs = e[1]
                strm = data[i + 32:i + 64]
                if len(strm) < 32:
                    break
                nlen = strm[3]
                name = ""
                j = i + 64
                for _ in range(max(0, secs - 1)):
                    ne = data[j:j + 32]
                    if len(ne) == 32 and ne[0] == 0xC1:
                        name += ne[2:32].decode("utf-16-le", "ignore")
                    j += 32
                yield ("file", {
                    "name": name[:nlen],
                    "isdir": bool(e[4] & 0x10),
                    "nofat": bool(strm[1] & 0x02),
                    "first": int.from_bytes(strm[0x14:0x18], "little"),
                    "size": int.from_bytes(strm[0x18:0x20], "little")})
                i += secs * 32
                continue
            i += 32

    res = {"fs": "exFAT", "sector_size": ss, "sectors_per_cluster": spc,
           "cluster_size": clsz, "total_clusters": n_clusters}

    rootdata = dirdata(root)
    bitmap = None
    for kind, ent in entries(rootdata):
        if kind == "bitmap" and not (ent["flags"] & 0x01):
            bitmap = ent
            break
    if bitmap:
        nbytes = min(bitmap["len"], (n_clusters + 7) // 8)
        bmp = bytearray()
        c = bitmap["first"]
        while len(bmp) < nbytes and 2 <= c < 0xFFFFFFF6:
            bmp += raw.read(c2o(c), clsz)
            c = nxt(c)
        bmp = bytes(bmp[:nbytes])
        res["_free_runs"] = _free_runs_from_flags(
            lambda i: not (bmp[i >> 3] >> (i & 7)) & 1, 2,
            min(n_clusters, len(bmp) * 8))
    else:
        res["_free_runs"] = []
        res["free_map_unavailable"] = True

    def walk(data, parts):
        want = parts[0].lower()
        for kind, ent in entries(data):
            if kind != "file" or ent["name"].lower() != want:
                continue
            if len(parts) == 1 and not ent["isdir"]:
                return ent
            if ent["isdir"] and len(parts) > 1:
                return walk(dirdata(ent["first"]), parts[1:])
        return None

    found = walk(rootdata, relpath.strip("/").split("/")) if relpath else None
    if relpath and not found:
        res["found"] = False
        return res
    if found:
        nclust = (found["size"] + clsz - 1) // clsz
        cl = chain(found["first"], nclust, found["nofat"])
        res.update({"found": True, "file_size": found["size"],
                    "clusters": nclust, "clusters_seen": len(cl),
                    "nofatchain": found["nofat"],
                    "extents": _runs_from_clusters(cl)})
    return res


def probe(device, relpath, need_bytes=0):
    """Raw-parse `device`, measuring `relpath` (may be '' for card facts only).

    `need_bytes` lets the free-space prediction run before the file exists —
    the cluster size it has to be divided by is only known after parsing.
    """
    raw = Raw(device)
    try:
        bs = raw.read(0, 512)
        if len(bs) < 512:
            raise Fatal("could not read a boot sector from %s" % device)
        if bs[3:11] == b"EXFAT   ":
            res = probe_exfat(raw, bs, relpath)
        elif bs[0x52:0x5A] == b"FAT32   " or int.from_bytes(bs[0x24:0x28], "little"):
            res = probe_fat32(raw, bs, relpath)
        else:
            raise Fatal("unrecognised filesystem on %s (not FAT32 or exFAT)" % device)
    finally:
        raw.close()

    free_runs = res.pop("_free_runs", [])
    clsz = res.get("cluster_size", 0) or 1
    need = max(res.get("clusters", 0), (need_bytes + clsz - 1) // clsz)
    res.update(_summarise_free(free_runs, need))
    res["need_clusters"] = need

    if res.get("found"):
        runs = len(res["extents"])
        res["runs"] = runs
        res["fat_end"] = RUN_HEADER + RUN_PAIR * runs
        res["margin"] = CTRL_WORDS_OFF - res["fat_end"]
        res["extents"] = res["extents"][:16]
    res["ok"] = True
    return res


# ==========================================================================
# privilege handling
# ==========================================================================
def probe_via_sudo(device, relpath, need_bytes=0, allow_prompt=True):
    """Run our own probe as root. Returns (result_dict, hint_command)."""
    me = os.path.abspath(__file__)
    cmd = [sys.executable, me, "--probe", device, relpath, str(need_bytes)]
    if os.geteuid() == 0:
        try:
            return probe(device, relpath, need_bytes), None
        except (Fatal, OSError) as exc:
            return {"ok": False, "error": str(exc)}, None
    try:
        return probe(device, relpath, need_bytes), None      # often user-readable
    except PermissionError:
        pass
    except (Fatal, OSError) as exc:
        return {"ok": False, "error": str(exc)}, None

    # quote every argument: relpath is legitimately empty on the pre-copy probe,
    # and an unquoted empty argument silently vanishes when pasted into a shell.
    hint = "sudo " + " ".join(shlex.quote(a) for a in cmd)
    cached = subprocess.run(["sudo", "-n", "true"],
                            capture_output=True).returncode == 0
    if not cached:
        if not (allow_prompt and sys.stdin.isatty()):
            return {"ok": False, "error": "needs root", "need_sudo": True}, hint
        info("raw partition access needs root; sudo will ask for your password once.")
        if subprocess.run(["sudo", "-v"]).returncode != 0:
            return {"ok": False, "error": "sudo refused", "need_sudo": True}, hint
    out = subprocess.run(["sudo"] + cmd, capture_output=True, text=True)
    if out.returncode != 0:
        return {"ok": False, "error": (out.stderr or out.stdout).strip(),
                "need_sudo": True}, hint
    try:
        return json.loads(out.stdout), None
    except ValueError:
        return {"ok": False, "error": "probe produced no JSON: %s" % out.stdout[:200],
                "need_sudo": True}, hint


# ==========================================================================
# macOS volume facts + safety guard
# ==========================================================================
def diskutil_plist(path):
    out = subprocess.run(["diskutil", "info", "-plist", path],
                         capture_output=True)
    if out.returncode != 0:
        raise Fatal("diskutil could not describe %s" % path)
    try:
        return plistlib.loads(out.stdout)
    except Exception as exc:
        raise Fatal("could not parse diskutil output for %s (%s)" % (path, exc))


def guard_volume(vol, image=None):
    """Refuse anything that is not an explicitly named removable FAT volume."""
    vol = os.path.realpath(vol)
    if vol in ("/", "/System", "/System/Volumes/Data"):
        raise Fatal("refusing to touch %s" % vol)
    if not os.path.isdir(vol):
        raise Fatal("not mounted / not a directory: %s" % vol)
    if not os.path.ismount(vol):
        raise Fatal("%s is not a mount point. Name the card's volume explicitly, "
                    "e.g. /Volumes/EZFLASH" % vol)
    if os.stat(vol).st_dev == os.stat("/").st_dev:
        raise Fatal("%s is on the SAME filesystem as / — that is a system disk, "
                    "refusing" % vol)
    if not vol.startswith("/Volumes/"):
        raise Fatal("%s is not under /Volumes — refusing (pass the card's volume "
                    "path)" % vol)

    if image:
        return {"device": None, "image": image, "fstype": "(image)",
                "name": os.path.basename(vol), "mount": vol,
                "free": shutil.disk_usage(vol).free,
                "total": shutil.disk_usage(vol).total,
                "personality": "(image)", "ejectable": True, "internal": False}

    d = diskutil_plist(vol)
    fstype = (d.get("FilesystemType") or "").lower()
    dev = d.get("DeviceIdentifier")
    if fstype not in ("msdos", "exfat"):
        raise Fatal("%s is %s, not FAT32/exFAT. An EZ-Flash card is FAT32 or "
                    "exFAT — refusing to write to this volume."
                    % (vol, fstype or d.get("FilesystemName", "unknown")))
    if not d.get("WritableVolume", True):
        raise Fatal("%s is mounted read-only" % vol)
    if not dev:
        raise Fatal("could not resolve a device identifier for %s" % vol)
    usage = shutil.disk_usage(vol)
    return {"device": dev, "image": None, "fstype": fstype, "mount": vol,
            "name": d.get("VolumeName") or os.path.basename(vol),
            "free": usage.free, "total": usage.total,
            "personality": d.get("FilesystemName", "?"),
            "ejectable": bool(d.get("Ejectable", False)),
            "internal": bool(d.get("Internal", False))}


def remount(card):
    """Unmount + remount: the only honest write barrier for removable media."""
    if card["image"]:
        # A hdiutil-attached image still has a real device; treat it the same.
        pass
    dev = card["device"]
    if not dev:
        raise Fatal("no device to remount")
    subprocess.run(["sync"])
    r = subprocess.run(["diskutil", "unmount", card["mount"]],
                       capture_output=True, text=True)
    if r.returncode != 0:
        warn("unmount refused — something is holding the volume open:")
        lsof = subprocess.run(["lsof", "+D", card["mount"]],
                              capture_output=True, text=True)
        for line in (lsof.stdout or "").splitlines()[:12]:
            info(line)
        raise Fatal("could not unmount %s; verification would read the page "
                    "cache, not the card" % card["mount"])
    r = subprocess.run(["diskutil", "mount", dev], capture_output=True, text=True)
    if r.returncode != 0:
        raise Fatal("remount of %s failed: %s" % (dev, r.stderr.strip()))
    d = diskutil_plist(dev)
    mp = d.get("MountPoint") or card["mount"]
    card["mount"] = mp
    return mp


# ==========================================================================
# source preflight + copy
# ==========================================================================
def preflight_source(src):
    if not os.path.isfile(src):
        raise Fatal("no such source file: %s" % src)
    st = os.stat(src)
    size = st.st_size
    allocated = st.st_blocks * 512
    hdr("SOURCE: %s" % src)
    info("size      : %s" % human(size))
    info("allocated : %s" % human(allocated))
    if size == 0:
        raise Fatal("source is zero bytes")
    if any(k in src for k in ("/CloudStorage/", "/OneDrive", "/iCloud",
                              "/Mobile Documents/", "/Google Drive", "/Dropbox/")):
        warn("source lives in a cloud-sync folder — placeholder/hydration risk")
    if allocated == 0:
        raise Fatal("DATALESS PLACEHOLDER: size>0 but zero blocks allocated. The "
                    "bytes are not on this Mac. Hydrate it first "
                    "(cat '%s' > /dev/null)." % src)
    if allocated < size * 9 // 10:
        raise Fatal("PARTIALLY HYDRATED: only %s of %s is local. Copying now "
                    "would truncate." % (human(allocated), human(size)))
    h = hashlib.sha256()
    crc = 0
    with open(src, "rb") as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                break
            h.update(b)
            crc = zlib.crc32(b, crc)
    ok("fully materialised; sha256 %s  crc32 %08X" % (h.hexdigest()[:16] + "...",
                                                      crc & 0xFFFFFFFF))
    return size, h.hexdigest(), crc & 0xFFFFFFFF


def hash_file(path):
    h = hashlib.sha256()
    crc = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                break
            h.update(b)
            crc = zlib.crc32(b, crc)
    return h.hexdigest(), crc & 0xFFFFFFFF


def rm_quiet(path):
    try:
        os.unlink(path)
        return True
    except OSError:
        return False


def copy_out(src, dst, size):
    d = os.path.dirname(dst)
    if d and not os.path.isdir(d):
        os.makedirs(d, exist_ok=True)
    t0 = time.time()
    done = 0
    try:
        with open(src, "rb") as fi, open(dst, "wb") as fo:
            while True:
                b = fi.read(CHUNK)
                if not b:
                    break
                fo.write(b)
                done += len(b)
                if _TTY and size:
                    pct = done * 100 // size
                    sys.stdout.write("\r       copying ... %3d%%" % pct)
                    sys.stdout.flush()
            fo.flush()
            os.fsync(fo.fileno())
    except OSError as exc:
        # ENOSPC arrives at write() or at close(); either way what is on the
        # card now is a truncated file. Remove it rather than leave a stub that
        # looks deployed.
        if _TTY:
            sys.stdout.write("\r" + " " * 40 + "\r")
        rm_quiet(dst)
        raise Fatal("write to the card failed after %s (%s). The partial copy "
                    "has been deleted. Free space and re-run."
                    % (human(done), exc))
    if _TTY:
        sys.stdout.write("\r" + " " * 40 + "\r")
        sys.stdout.flush()
    dt = max(time.time() - t0, 1e-6)
    ok("wrote %s in %.1fs (%.1f MiB/s), fsync'd"
       % (human(size), dt, size / 1048576.0 / dt))


def tidy_card(mount, dest):
    """Remove macOS metadata that fragments the card, and opt out of Spotlight."""
    removed = []
    for name in (".Spotlight-V100", ".fseventsd", ".Trashes", ".TemporaryItems"):
        p = os.path.join(mount, name)
        if os.path.exists(p):
            try:
                shutil.rmtree(p, ignore_errors=True)
                removed.append(name)
            except OSError:
                pass
    for root, _dirs, files in os.walk(mount):
        for fn in files:
            if fn.startswith("._"):
                if rm_quiet(os.path.join(root, fn)):
                    removed.append(os.path.relpath(os.path.join(root, fn), mount))
    marker = os.path.join(mount, ".metadata_never_index")
    if not os.path.exists(marker):
        try:
            open(marker, "wb").close()
            removed.append("+.metadata_never_index")
        except OSError:
            pass
    if removed:
        ok("tidied: %s" % ", ".join(removed[:8]) +
           (" (+%d more)" % (len(removed) - 8) if len(removed) > 8 else ""))
    else:
        ok("nothing to tidy")
    # the AppleDouble sidecar for our own destination, always
    side = os.path.join(mount, os.path.dirname(dest),
                        "._" + os.path.basename(dest))
    rm_quiet(side)


def write_balloon(mount, nbytes):
    """Fill the low free holes so the next allocation lands in the big run."""
    path = os.path.join(mount, BALLOON_NAME)
    zero = b"\0" * CHUNK
    written = 0
    try:
        with open(path, "wb") as f:
            while written < nbytes:
                n = min(CHUNK, nbytes - written)
                f.write(zero[:n])
                written += n
            f.flush()
            os.fsync(f.fileno())
    except OSError as exc:
        rm_quiet(path)
        raise Fatal("balloon write failed (%s) — card may be full" % exc)
    return path


# ==========================================================================
# reporting
# ==========================================================================
def report_card(card, res, need_bytes):
    hdr("CARD: %s (%s)" % (card["mount"], card["device"] or card["image"]))
    info("filesystem     : %s   personality %s" % (res.get("fs", card["fstype"]),
                                                   card["personality"]))
    clsz = res.get("cluster_size", 0)
    if clsz:
        info("cluster size   : %d bytes (%d KiB), %d sectors/cluster of %d B"
             % (clsz, clsz // 1024, res.get("sectors_per_cluster", 0),
                res.get("sector_size", 0)))
        need_cl = (need_bytes + clsz - 1) // clsz
        info("your ROM needs : %d clusters (%s)" % (need_cl, human(need_bytes)))
        info("                 a smaller cluster size means more clusters and so")
        info("                 more ways for the file to break into fragments.")
    info("space          : %s free of %s" % (human(card["free"]), human(card["total"])))
    if not card["ejectable"]:
        warn("diskutil says this volume is NOT ejectable — double-check it is the card")
    if card["internal"]:
        warn("diskutil says this device is Internal (built-in SD slot reports this too)")

    if res.get("free_run_count") is not None and clsz:
        big = res["largest_free_run_clusters"] * clsz
        info("free space map : %d free runs; largest contiguous run %s"
             % (res["free_run_count"], human(big)))
        need_cl = (need_bytes + clsz - 1) // clsz
        if res["largest_free_run_clusters"] < need_cl:
            bad("NO contiguous free run is big enough for this ROM. Every copy "
                "WILL be fragmented until you free space or reformat.")
        pred = res.get("firstfit_predicted_extents", 0)
        if pred == -1:
            bad("the file does not fit in the free space at all")
        elif pred:
            note = "within budget" if pred <= MAX_SAFE_RUNS else "OVER BUDGET"
            info("prediction     : a naive first-fit copy would take ~%d extents "
                 "(%s)" % (pred, note))


def verdict(res):
    """Print the run-table verdict. Returns True if within the kernel's budget."""
    runs = res["runs"]
    fat_end = res["fat_end"]
    margin = res["margin"]
    hdr("KERNEL RUN TABLE")
    info("extents on card : %d" % runs)
    info("buffer used     : 8 + 8*%d = 0x%03X bytes  (ezkernelnew.c:993-1013)"
         % (runs, fat_end))
    info("FPGA ctrl words : 0x%03X  (ezkernelnew.c:1798-1801)" % CTRL_WORDS_OFF)
    info("margin          : %+d bytes  (= %d spare fragments)"
         % (margin, margin // RUN_PAIR))
    for i, (start, count) in enumerate(res.get("extents", [])[:8]):
        info("  [%2d] cluster %-10d x %d" % (i, start, count))
    if runs > len(res.get("extents", [])):
        info("  ... (%d extents total)" % runs)

    if margin >= 0x100:
        ok("SAFE — run list ends far below the FPGA control words (<= %d extents "
           "is wide open)." % WIDE_SAFE_RUNS)
        return True
    if margin >= 0:
        warn("SAFE BUT THIN — only %d bytes of headroom (%d more fragments and "
             "the control words are destroyed)." % (margin, margin // RUN_PAIR))
        return True
    if margin >= -0x10:
        bad("CTRL-WORD OVERWRITE — the run list reaches into 0x%03X. The FPGA "
            "gets a garbage file size / copy mode and Send_FATbuffer waits for "
            "it forever. THIS FILE WILL HANG AT 'LOADING GAME'." % CTRL_WORDS_OFF)
        return False
    bad("SEVERE — the run list runs past 0x200, destroying the control words "
        "and the RTS region as well. THIS FILE WILL HANG.")
    return False


# ==========================================================================
# main flow
# ==========================================================================
def card_devices(card):
    """Raw nodes to try, best first.

    /dev/rdiskNsM is the character device: it bypasses the buffer cache (so it
    cannot hand back a stale view of what we just wrote) and, for removable
    media owned by the console user, it is readable WITHOUT root. Reads through
    it must be block-aligned, which Raw already guarantees.

    /dev/diskNsM is the buffered block device. macOS returns EBUSY on it while
    the volume is mounted, so it is only a fallback.
    """
    if card["image"]:
        return [card["image"]]
    return ["/dev/r" + card["device"], "/dev/" + card["device"]]


def measure(card, dest, need_bytes=0, allow_prompt=True):
    tried = []
    for device in card_devices(card):
        res, hint = probe_via_sudo(device, dest, need_bytes,
                                   allow_prompt=allow_prompt)
        if res.get("ok"):
            return res, hint, device
        tried.append((res, hint, device))
    if not tried:
        return {"ok": False, "error": "no device"}, None, None
    # a "needs root" answer is more actionable than a later EBUSY
    for entry in tried:
        if entry[0].get("need_sudo"):
            return entry
    return tried[0]


def do_deploy(args):
    src = os.path.abspath(args.source) if args.source else None
    dest = args.dest_name or (os.path.basename(src) if src else None)

    card = guard_volume(args.volume, image=args.image)
    hdr("SAFETY GUARD")
    ok("target is %s on %s (%s, %s) — not the root filesystem"
       % (card["mount"], card["device"] or card["image"], card["fstype"],
          "ejectable" if card["ejectable"] else "NOT ejectable"))

    size = 0
    src_sha = src_crc = None
    if src:
        size, src_sha, src_crc = preflight_source(src)

    # card facts + free map, before touching anything
    res0, hint, device = measure(card, dest if args.check_only else "", size)
    probe_ok = res0.get("ok")
    probe_need_sudo = res0.get("need_sudo")
    probe_error = res0.get("error")
    if not probe_ok:
        if probe_need_sudo:
            warn("cannot read the raw partition without root.")
            info("Run exactly this to get the fragmentation number:")
            info("    %s" % hint)
        else:
            warn("card probe failed: %s" % probe_error)
        res0 = {}
    report_card(card, res0, size or 1)

    if args.tidy and not args.dry_run:
        hdr("TIDY (macOS metadata)")
        tidy_card(card["mount"], dest or "")

    stale = os.path.join(card["mount"], BALLOON_NAME)
    if os.path.exists(stale) and not args.dry_run:
        warn("removing stale defrag balloon from an interrupted run")
        rm_quiet(stale)

    # ---------------- check-only ----------------
    if args.check_only:
        # A probe failure is NOT evidence the file is missing — res0 was reset to {}
        # above precisely because the raw walk never got to look, so res0.get("found")
        # would be falsy here for the exact same reason it would be for a genuinely
        # absent file. Ask `probe_ok` (captured before that reset) first, and only
        # trust "found" once the probe actually ran. Conflating the two used to print
        # "is not on the card" — and this instruction sheet's exit 1 tells the user
        # nothing was written and to re-copy — for the needs-root case, which
        # destroys the very specimen a re-run was trying to measure.
        if not probe_ok:
            if probe_need_sudo:
                warn("cannot read the raw partition without root, so presence "
                     "could not be measured.")
                info("Run exactly this to get the fragmentation number:")
                info("    %s" % hint)
            else:
                warn("card probe failed: %s" % probe_error)
            bad("presence of %s could NOT be determined — the probe failed, "
                "not the file." % dest)
            info("This is NOT evidence the file is missing. Do not re-copy the ROM.")
            return 4
        if not res0.get("found"):
            bad("%s is not on the card" % dest)
            return 1
        target = os.path.join(card["mount"], dest)
        info("on-card size  : %s" % human(res0["file_size"]))
        good = verdict(res0)
        if src and os.path.isfile(target):
            hdr("BYTES")
            warn("this file was not re-read through an unmount/remount, so the "
                 "hash below may come from the Mac's page cache")
            dsh, dcrc = hash_file(target)
            if dsh == src_sha:
                ok("sha256 matches the source")
            else:
                bad("sha256 DIFFERS from the source (card %08X vs src %08X)"
                    % (dcrc, src_crc))
                return 2
        return 0 if good else 3

    if args.dry_run:
        hdr("DRY RUN")
        info("would copy   : %s" % src)
        info("           -> : %s" % os.path.join(card["mount"], dest))
        info("would then unmount/remount, re-hash off the media, and measure the")
        info("run table against the 0x%03X budget (max %d extents)."
             % (CTRL_WORDS_OFF, MAX_SAFE_RUNS))
        if size > card["free"]:
            bad("NOT ENOUGH FREE SPACE: need %s, have %s"
                % (human(size), human(card["free"])))
            return 1
        ok("nothing was written")
        return 0

    if size > card["free"]:
        raise Fatal("not enough free space: need %s, have %s"
                    % (human(size), human(card["free"])))

    # ---------------- copy / verify / measure loop ----------------
    attempts = max(1, args.retries + 1)
    last = None
    for attempt in range(1, attempts + 1):
        hdr("ATTEMPT %d of %d" % (attempt, attempts))
        target = os.path.join(card["mount"], dest)

        if os.path.exists(target):
            info("deleting the existing copy first (frees its clusters, which is "
                 "what makes a re-copy defragment)")
            if not rm_quiet(target):
                raise Fatal("could not delete %s" % target)
        rm_quiet(os.path.join(card["mount"], os.path.dirname(dest),
                              "._" + os.path.basename(dest)))
        subprocess.run(["sync"])

        # Defrag pass. Worth doing on the very first attempt too, when the
        # pre-copy prediction already says a plain copy will not fit the budget.
        predicted = res0.get("firstfit_predicted_extents") or 0
        balloon = None
        if args.defrag and (attempt > 1 or predicted > MAX_SAFE_RUNS):
            # The free map must be read AFTER the delete above and after that
            # delete has reached the media — we read the raw device, which does
            # not see anything still sitting in the kernel's cache. A remount is
            # the only guaranteed flush, so pay for one here.
            hdr("DEFRAG PASS")
            info("re-reading the free-space map now that the old copy is gone")
            remount(card)
            fresh, _h, _d = measure(card, "", size)
            if not fresh.get("ok") or not fresh.get("cluster_size"):
                warn("could not re-read the free map; skipping the defrag pass")
                fresh = {}
            clsz = fresh.get("cluster_size", 0)
            cost = fresh.get("clusters_before_largest", 0) * clsz
            biggest = fresh.get("largest_free_run_clusters", 0) * clsz
            cap = args.balloon_max * 1048576
            free_now = shutil.disk_usage(card["mount"]).free
            if not clsz:
                pass
            elif biggest < size:
                warn("largest contiguous free run is %s, smaller than the ROM's "
                     "%s — no allocation trick can make this contiguous"
                     % (human(biggest), human(size)))
            elif cost == 0:
                info("the largest free run is already first in line; a plain "
                     "copy should land in it")
            elif cost > cap:
                warn("reaching the %s run would mean writing %s of filler, over "
                     "the --balloon-max %d MiB cap — skipping"
                     % (human(biggest), human(cost), args.balloon_max))
            elif cost + size + (16 << 20) > free_now:
                warn("a balloon pass would leave too little free space, skipping")
            else:
                info("filling %s of small free holes so the ROM lands in the %s "
                     "contiguous run (best effort — judged only by the "
                     "measurement below)" % (human(cost), human(biggest)))
                balloon = write_balloon(card["mount"], cost)
            target = os.path.join(card["mount"], dest)

        try:
            copy_out(src, target, size)
        finally:
            if balloon:
                info("removing the defrag balloon")
                rm_quiet(balloon)

        hdr("FLUSH BARRIER (unmount + remount)")
        info("macOS `sync` does not flush a removable device's own write cache,")
        info("and without a remount the verify would read the page cache — i.e.")
        info("hand back the bytes we just wrote from RAM and call that proof.")
        mp = remount(card)
        ok("remounted at %s" % mp)
        target = os.path.join(mp, dest)
        if not os.path.isfile(target):
            bad("the file is not on the card after the remount")
            return 2

        hdr("BYTES (read back off the media)")
        dsz = os.path.getsize(target)
        info("size src : %s" % human(size))
        info("size dst : %s" % human(dsz))
        if dsz != size:
            bad("SIZE MISMATCH — the card's copy is truncated.")
            info("VERDICT: BYTES BAD. This is a copy failure, not a load failure.")
            info("Remedy: re-run; if it repeats, the card or reader is faulty — "
                 "try another card before touching the ROM.")
            return 2
        dsh, dcrc = hash_file(target)
        info("sha256 src : %s" % src_sha)
        info("sha256 dst : %s" % dsh)
        info("crc32  src : %08X    dst : %08X" % (src_crc, dcrc))
        if dsh != src_sha:
            bad("CONTENT MISMATCH — the card's copy differs from the source.")
            info("VERDICT: BYTES BAD. Do NOT blame fragmentation or the ROM.")
            info("Remedy: re-run this tool; if it repeats, the card is bad "
                 "(f3write/f3read it) or the source was a cloud placeholder.")
            return 2
        ok("byte-for-byte identical after a real media round-trip")

        hdr("MEASURE")
        res, hint, device = measure(card, dest, size)
        if not res.get("ok"):
            if res.get("need_sudo"):
                warn("bytes are verified, but fragmentation was NOT measured.")
                info("Run exactly this and read the margin line:")
                info("    %s" % hint)
                return 4
            warn("probe failed: %s" % res.get("error"))
            return 4
        if not res.get("found"):
            warn("the raw FAT walk could not find %s (name mangling?)" % dest)
            return 4
        res0 = res
        last = res
        good = verdict(res)
        if good and res["runs"] <= args.retry_above:
            hdr("RESULT")
            ok("DEPLOYED AND VERIFIED — %d extents, %+d bytes of margin. This "
               "file will load from SD." % (res["runs"], res["margin"]))
            finish(card, args)
            emit_json(args, card, dest, res, "OK", size, src_sha, src_crc)
            return 0
        more = attempt < attempts
        if good:
            warn("within the hard budget but above --retry-above %d%s"
                 % (args.retry_above,
                    "; retrying for more headroom" if more else ""))
        elif more:
            warn("over budget — retrying (a delete + re-copy usually defragments)")
        else:
            warn("over budget and out of attempts")

    # ---------------- gave up ----------------
    hdr("RESULT")
    if last and last["margin"] >= 0:
        warn("STOPPING WITH THIN MARGIN: %d extents, %+d bytes (%d spare "
             "fragments)." % (last["runs"], last["margin"],
                              last["margin"] // RUN_PAIR))
        info("The bytes are correct and it is inside the kernel's budget, so it "
             "should load — but there is little room left.")
        finish(card, args)
        emit_json(args, card, dest, last, "THIN", size, src_sha, src_crc)
        return 0

    bad("COULD NOT GET THIS FILE UNDER THE KERNEL'S BUDGET after %d attempts."
        % attempts)
    if last:
        clsz = last.get("cluster_size", 0)
        info("measured: %d extents, buffer end 0x%03X, margin %+d (budget: at "
             "most %d extents)" % (last["runs"], last["fat_end"], last["margin"],
                                   MAX_SAFE_RUNS))
        if clsz:
            info("cluster size %d KiB; largest contiguous free run %s; free "
                 "space split across %d runs"
                 % (clsz // 1024,
                    human(last.get("largest_free_run_clusters", 0) * clsz),
                    last.get("free_run_count", 0)))
    info("")
    info("VERDICT: FRAGMENTED. The bytes on the card are correct — this is purely")
    info("where they landed. Do ONE of these, in order of effort:")
    info("  1. Free space: delete files you do not need, empty /.sdtrash and")
    info("     /.Trashes, then re-run. Fragmentation is a symptom of a full card.")
    info("  2. Back the card up and reformat it FAT32 with the LARGEST cluster")
    info("     size it offers (32 KiB or 64 KiB). Fewer clusters per file means")
    info("     fewer possible fragments; a 12.5 MB ROM at 64 KiB is 191 clusters.")
    info("  3. Copy this ROM FIRST onto the freshly formatted card, before")
    info("     anything else — a file written to an empty card is contiguous.")
    info("  4. Until then, load it from NOR. The NOR path never builds this run")
    info("     table, which is exactly why NOR has always worked.")
    emit_json(args, card, dest, last, "FRAGMENTED", size, src_sha, src_crc)
    return 3


def finish(card, args):
    if args.eject:
        subprocess.run(["sync"])
        r = subprocess.run(["diskutil", "eject", card["device"] or card["mount"]],
                           capture_output=True, text=True)
        if r.returncode == 0:
            ok("ejected — safe to pull the card")
        else:
            warn("eject failed: %s" % r.stderr.strip())
    else:
        info("Eject before pulling the card:  diskutil eject %s"
             % (card["device"] or card["mount"]))


def emit_json(args, card, dest, res, status, size, sha, crc):
    if not args.json:
        return
    out = {"status": status, "volume": card["mount"], "device": card["device"],
           "dest": dest, "size": size, "sha256": sha, "crc32": crc,
           "max_safe_runs": MAX_SAFE_RUNS, "ctrl_words_off": CTRL_WORDS_OFF}
    if res:
        out.update({k: res.get(k) for k in
                    ("fs", "cluster_size", "clusters", "runs", "fat_end",
                     "margin", "free_run_count", "largest_free_run_clusters")})
    with open(args.json, "w") as f:
        json.dump(out, f, indent=2)
    info("wrote %s" % args.json)


def main(argv):
    # hidden: re-entry as root for the raw probe
    if len(argv) >= 2 and argv[0] == "--probe":
        try:
            rel = argv[2] if len(argv) > 2 else ""
            need = int(argv[3]) if len(argv) > 3 else 0
            print(json.dumps(probe(argv[1], rel, need)))
            return 0
        except (Fatal, OSError) as exc:
            print(json.dumps({"ok": False, "error": str(exc)}))
            return 1

    p = argparse.ArgumentParser(
        prog="sd_deploy.py",
        description="Copy a .gba to an EZ-Flash microSD and prove the card can "
                    "load it (bytes verified + kernel run-table margin measured).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="exit: 0 ok | 1 usage/guard | 2 BYTES BAD | 3 FRAGMENTED | "
               "4 UNMEASURED (no root)")
    p.add_argument("source", nargs="?", help="the .gba to deploy")
    p.add_argument("volume", nargs="?", help="the card, e.g. /Volumes/EZFLASH")
    p.add_argument("--as", dest="dest_name", metavar="NAME",
                   help="destination name on the card (default: source basename)")
    p.add_argument("--check-only", action="store_true",
                   help="measure a file already on the card; copy nothing. "
                        "Usage: --check-only VOLUME NAME.gba")
    p.add_argument("--dry-run", action="store_true",
                   help="inspect the card and predict, write nothing")
    p.add_argument("--retries", type=int, default=2,
                   help="extra copy attempts if over budget (default 2)")
    p.add_argument("--retry-above", type=int, default=45, metavar="N",
                   help="retry even when technically safe, if extents exceed N "
                        "(default 45; the hard budget is %d)" % MAX_SAFE_RUNS)
    p.add_argument("--no-defrag", dest="defrag", action="store_false",
                   help="never write the temporary balloon file used to steer "
                        "the allocator into the largest free run")
    p.add_argument("--balloon-max", type=int, default=256, metavar="MiB",
                   help="cap on balloon bytes written during a defrag pass "
                        "(default 256)")
    p.add_argument("--tidy", action="store_true",
                   help="remove .Spotlight-V100/.fseventsd/.Trashes/._* and add "
                        ".metadata_never_index")
    p.add_argument("--eject", action="store_true", help="eject when done")
    p.add_argument("--json", metavar="FILE", help="write a machine-readable summary")
    p.add_argument("--image", metavar="FILE",
                   help="measure this raw image file instead of the mounted "
                        "device (for testing this tool against a synthetic card)")
    args = p.parse_args(argv)

    if args.check_only:
        # --check-only VOLUME NAME.gba  (source slot holds the volume)
        if args.source and args.volume and not args.dest_name:
            args.volume, args.dest_name = args.source, args.volume
            args.source = None
        if not args.volume or not args.dest_name:
            p.error("--check-only needs: VOLUME NAME.gba")
    else:
        if not args.source or not args.volume:
            p.error("need: SOURCE.gba /Volumes/CARDNAME")

    try:
        return do_deploy(args)
    except Fatal as exc:
        bad(str(exc))
        return 1
    except KeyboardInterrupt:
        bad("interrupted — the card may hold a partial copy; re-run before use")
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
