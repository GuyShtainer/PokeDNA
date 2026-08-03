#!/usr/bin/env python3
"""
Reference renderer + ground-truth dump for the PokeDNA map viewer.

Renders a Gen-3 overworld map straight out of a retail ROM and writes:
  - a PNG so a human can SEE whether the decode is right (noise vs a town), and
  - a JSON of intermediate values so the C implementation in source/map_render.c
    can be asserted against exactly the same numbers.

This is deliberately a SECOND, independent implementation of the same spec the C
core implements. Two implementations agreeing on real ROM bytes is the only cheap
way to be confident about a pixel pipeline before it reaches a GBA screen.

The ROM is the user's own dump and is NEVER committed; pass its path in.

Usage:
  python3 tools/gen_render_truth.py <rom.gba> [--out docs/analysis-2026-07-29]

Spec implemented (all verified in docs/analysis-2026-07-29/feature3b-rom-map-data.md):
  block entry u16 : bits 0-9 metatile id, 10-11 collision, 12-15 elevation
  metatile        : 8 u16 = bottom layer (TL,TR,BL,BR) then top layer (TL,TR,BL,BR)
  tilemap entry   : bits 0-9 tile id, bit10 hflip, bit11 vflip, bits 12-15 palette
  tiles           : 4bpp, 32 B per 8x8 tile, 4 B per row, LOW nibble = left pixel
  palette         : BGR555 u16, bits 0-4 R, 5-9 G, 10-14 B  (GBA-native, no swizzle)
  primary/secondary: tile id and metatile id split at 512 (640 on FRLG);
                     palette slots 0-5 from primary, 6-12 read from the SECONDARY
                     tileset's own array AT THE SAME INDEX (not index-6) -- the trap
  colour index 0  : transparent in the TOP layer (bottom shows through)
"""
import json
import os
import struct
import sys
import zlib

ROM_BASE = 0x08000000
BACKDROP = 0x0000   # BG palette entry 0, forced to RGB_BLACK by the game

# per-version anchors (same table as source/rom_map.c)
VERSIONS = {
    (b"BPEE", 0): dict(name="Emerald",   groups=34, map_groups=0x08486578, prim_tiles=512, prim_pals=6, layout_size=24, ts_attr=0x10),
    (b"AXVE", 1): dict(name="Ruby",      groups=34, map_groups=0x083085A0, prim_tiles=512, prim_pals=6, layout_size=24, ts_attr=0x10),
    (b"AXVE", 2): dict(name="Ruby",      groups=34, map_groups=0x083085A0, prim_tiles=512, prim_pals=6, layout_size=24, ts_attr=0x10),
    (b"BPRE", 1): dict(name="FireRed",   groups=43, map_groups=0x08352718, prim_tiles=640, prim_pals=7, layout_size=28, ts_attr=0x14),
}


class Rom:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        self.code = self.d[0xAC:0xB0]
        self.rev = self.d[0xBC]
        key = (self.code, self.rev)
        if key not in VERSIONS:
            raise SystemExit("unsupported ROM %r rev %d" % (self.code, self.rev))
        self.v = VERSIONS[key]

    def at(self, addr):
        off = addr - ROM_BASE
        if not (0 <= off < len(self.d)):
            raise ValueError("pointer 0x%08X outside ROM" % addr)
        return off

    def u8(self, a):  return self.d[self.at(a)]
    def u16(self, a): return struct.unpack_from("<H", self.d, self.at(a))[0]
    def u32(self, a): return struct.unpack_from("<I", self.d, self.at(a))[0]
    def s32(self, a): return struct.unpack_from("<i", self.d, self.at(a))[0]
    def raw(self, a, n):
        o = self.at(a)
        return self.d[o:o + n]

    # ---- group table: counts derived from consecutive pointer differences ----
    def groups(self):
        n = self.v["groups"]
        ptrs = [self.u32(self.v["map_groups"] + i * 4) for i in range(n)]
        counts = []
        for i in range(n):
            end = ptrs[i + 1] if i + 1 < n else self.v["map_groups"]
            counts.append((end - ptrs[i]) // 4)
        return ptrs, counts

    def map_header(self, group, num):
        ptrs, counts = self.groups()
        if not (0 <= group < len(ptrs)) or not (0 <= num < counts[group]):
            raise SystemExit("no such map %d.%d" % (group, num))
        h = self.u32(ptrs[group] + num * 4)
        return dict(
            addr=h,
            layout=self.u32(h + 0x00), events=self.u32(h + 0x04),
            scripts=self.u32(h + 0x08), connections=self.u32(h + 0x0C),
            music=self.u16(h + 0x10), layout_id=self.u16(h + 0x12),
            mapsec=self.u8(h + 0x14), cave=self.u8(h + 0x15),
            weather=self.u8(h + 0x16), map_type=self.u8(h + 0x17),
        )

    def layout(self, addr):
        return dict(
            addr=addr,
            width=self.s32(addr + 0x00), height=self.s32(addr + 0x04),
            border=self.u32(addr + 0x08), blocks=self.u32(addr + 0x0C),
            primary=self.u32(addr + 0x10), secondary=self.u32(addr + 0x14),
        )

    def tileset(self, addr):
        return dict(
            addr=addr,
            compressed=self.u8(addr + 0x00), secondary=self.u8(addr + 0x01),
            tiles=self.u32(addr + 0x04), palettes=self.u32(addr + 0x08),
            metatiles=self.u32(addr + 0x0C),
            attributes=self.u32(addr + self.v["ts_attr"]),
        )


def lz77(rom, addr, cap=1 << 20):
    """GBA BIOS LZ77 (LZ10). Header: byte0 0x10, bytes1-3 = decompressed size LE."""
    hdr = rom.u32(addr)
    if (hdr & 0xFF) != 0x10:
        raise ValueError("not LZ77 at 0x%08X (type byte 0x%02X)" % (addr, hdr & 0xFF))
    size = hdr >> 8
    if size > cap:
        raise ValueError("decompressed size %d over cap" % size)
    src = rom.at(addr) + 4
    d = rom.d
    out = bytearray()
    while len(out) < size:
        flags = d[src]; src += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                b1, b2 = d[src], d[src + 1]; src += 2
                length = (b1 >> 4) + 3
                disp = (((b1 & 0x0F) << 8) | b2) + 1
                start = len(out) - disp
                if start < 0:
                    raise ValueError("LZ77 back-reference before start")
                for k in range(length):
                    out.append(out[start + k])          # byte-wise: overlaps are legal
            else:
                out.append(d[src]); src += 1
    return bytes(out[:size]), size


def tiles_of(rom, ts):
    """Decompressed 4bpp char data for a tileset (or raw bytes if uncompressed)."""
    if ts["tiles"] == 0:
        return b""
    if ts["compressed"]:
        data, _ = lz77(rom, ts["tiles"])
        return data
    # uncompressed: no length field; take a generous slice, callers index by tile
    return rom.raw(ts["tiles"], 1024 * 32)


def palettes_of(rom, ts):
    """16 palettes x 16 BGR555 colours."""
    if ts["palettes"] == 0:
        return [[0] * 16 for _ in range(16)]
    raw = rom.raw(ts["palettes"], 16 * 16 * 2)
    return [list(struct.unpack_from("<16H", raw, p * 32)) for p in range(16)]


def bgr555_to_rgb888(c):
    r = (c & 0x1F) << 3
    g = ((c >> 5) & 0x1F) << 3
    b = ((c >> 10) & 0x1F) << 3
    return (r | r >> 5, g | g >> 5, b | b >> 5)


class Renderer:
    def __init__(self, rom, lay):
        self.rom, self.lay = rom, lay
        self.prim = rom.tileset(lay["primary"])
        self.sec = rom.tileset(lay["secondary"]) if lay["secondary"] else None
        self.prim_tiles = tiles_of(rom, self.prim)
        self.sec_tiles = tiles_of(rom, self.sec) if self.sec else b""
        self.prim_pals = palettes_of(rom, self.prim)
        self.sec_pals = palettes_of(rom, self.sec) if self.sec else [[0] * 16] * 16
        self.split = rom.v["prim_tiles"]          # 512 / 640
        self.pal_split = rom.v["prim_pals"]       # 6 / 7

    def block(self, x, y):
        return self.rom.u16(self.lay["blocks"] + 2 * (y * self.lay["width"] + x))

    def metatile_entries(self, mid):
        """The 8 raw u16 tilemap entries for a metatile id."""
        if mid < self.split:
            base, idx = self.prim["metatiles"], mid
        else:
            base, idx = self.sec["metatiles"], mid - self.split
        return list(struct.unpack_from("<8H", self.rom.raw(base + idx * 16, 16), 0))

    def attr(self, mid):
        if mid < self.split:
            base, idx = self.prim["attributes"], mid
        else:
            base, idx = self.sec["attributes"], mid - self.split
        return self.rom.u16(base + idx * 2)

    def _tile_bytes(self, tid):
        if tid < self.split:
            src, i = self.prim_tiles, tid
        else:
            src, i = self.sec_tiles, tid - self.split
        o = i * 32
        return src[o:o + 32] if o + 32 <= len(src) else b"\0" * 32

    def _pal(self, p):
        # THE TRAP: slots >= NUM_PALS_IN_PRIMARY come from the SECONDARY tileset's
        # own palette array at the SAME index, not at (index - split).
        return self.prim_pals[p] if p < self.pal_split else self.sec_pals[p]

    def draw_tile(self, dst, dw, ox, oy, entry, transparent):
        tid = entry & 0x03FF
        hf = (entry >> 10) & 1
        vf = (entry >> 11) & 1
        pal = self._pal((entry >> 12) & 0x0F)
        tb = self._tile_bytes(tid)
        for ty in range(8):
            sy = 7 - ty if vf else ty
            row = tb[sy * 4:sy * 4 + 4]
            for tx in range(8):
                sx = 7 - tx if hf else tx
                byte = row[sx >> 1]
                ci = (byte & 0x0F) if (sx & 1) == 0 else (byte >> 4)
                if transparent and ci == 0:
                    continue
                dst[(oy + ty) * dw + (ox + tx)] = pal[ci]

    def draw_metatile(self, dst, dw, ox, oy, mid):
        e = self.metatile_entries(mid)
        # Colour 0 is transparent on EVERY layer, bottom included; what shows through is
        # the backdrop, which the game forces to black in LoadTilesetPalette (it
        # overwrites palette entry 0 with RGB_BLACK and never shows the tileset's own
        # colour 0). Painting the tileset's colour 0 for the bottom layer -- which this
        # script and map_render.c both used to do -- is wrong on ~0.6% of metatiles.
        for y in range(16):
            for x in range(16):
                dst[(oy + y) * dw + (ox + x)] = BACKDROP
        for li in (0, 4):
            for q in range(4):                      # TL, TR, BL, BR
                self.draw_tile(dst, dw, ox + (q & 1) * 8, oy + (q >> 1) * 8,
                               e[li + q], True)

    def render(self, x0, y0, w, h):
        dw, dh = w * 16, h * 16
        buf = [0] * (dw * dh)
        for by in range(h):
            for bx in range(w):
                cell = self.block(x0 + bx, y0 + by)
                self.draw_metatile(buf, dw, bx * 16, by * 16, cell & 0x03FF)
        return buf, dw, dh


def write_png(path, pix, w, h):
    """Minimal RGB8 PNG writer (no PIL dependency)."""
    rows = bytearray()
    for y in range(h):
        rows.append(0)                              # filter type 0
        for x in range(w):
            r, g, b = bgr555_to_rgb888(pix[y * w + x])
            rows += bytes((r, g, b))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def find_by_layout_id(rom, want):
    ptrs, counts = rom.groups()
    for g in range(len(ptrs)):
        for m in range(counts[g]):
            h = rom.map_header(g, m)
            if h["layout_id"] == want:
                return g, m
    return None


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    rom = Rom(sys.argv[1])
    out = "docs/analysis-2026-07-29"
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
    os.makedirs(out, exist_ok=True)

    print("ROM %s rev%d -> %s" % (rom.code.decode(), rom.rev, rom.v["name"]))
    ptrs, counts = rom.groups()
    print("groups=%d maps=%d group0=%d" % (len(ptrs), sum(counts), counts[0]))

    # Littleroot is layout id 10 (1-based into gMapLayouts); find it rather than assume.
    targets = []
    lit = find_by_layout_id(rom, 10)
    if lit:
        targets.append(("littleroot", lit[0], lit[1]))
    targets.append(("petalburg", 0, 0))             # 30x30, known spot-check in the docs

    truth = {"rom": {"code": rom.code.decode(), "rev": rom.rev, "name": rom.v["name"],
                     "groups": len(ptrs), "maps": sum(counts), "group0": counts[0]},
             "maps": {}}

    for label, g, m in targets:
        h = rom.map_header(g, m)
        lay = rom.layout(h["layout"])
        r = Renderer(rom, lay)
        print("\n%s: map %d.%d  layout 0x%08X  %dx%d  mapsec=%d type=%d"
              % (label, g, m, lay["addr"], lay["width"], lay["height"],
                 h["mapsec"], h["map_type"]))
        print("  primary  0x%08X comp=%d tiles=0x%08X -> %d B (%d tiles)"
              % (r.prim["addr"], r.prim["compressed"], r.prim["tiles"],
                 len(r.prim_tiles), len(r.prim_tiles) // 32))
        if r.sec:
            print("  secondary 0x%08X comp=%d tiles=0x%08X -> %d B (%d tiles)"
                  % (r.sec["addr"], r.sec["compressed"], r.sec["tiles"],
                     len(r.sec_tiles), len(r.sec_tiles) // 32))

        w = min(lay["width"], 20)
        hh = min(lay["height"], 20)
        pix, dw, dh = r.render(0, 0, w, hh)
        png = os.path.join(out, "render-%s.png" % label)
        write_png(png, pix, dw, dh)
        print("  wrote %s (%dx%d px)" % (png, dw, dh))

        # Raw BGR555 dump for the C host test to byte-compare against. Header is
        # (u16 group, u16 num, u16 w_px, u16 h_px) then w*h u16 pixels, all LE.
        raw = os.path.join(out, "render-%s.raw" % label)
        with open(raw, "wb") as rf:
            rf.write(struct.pack("<4H", g, m, dw, dh))
            rf.write(struct.pack("<%dH" % (dw * dh), *pix))
        print("  wrote %s (%d B)" % (raw, 8 + dw * dh * 2))

        c00 = r.block(0, 0)
        ccx = r.block(lay["width"] // 2, lay["height"] // 2)
        mid0 = c00 & 0x03FF
        # a 16x16 block of composited pixels for cell (0,0), as hex, for the C test
        blk = [0] * 256
        r.draw_metatile(blk, 16, 0, 0, mid0)

        truth["maps"][label] = {
            "group": g, "num": m,
            "header": {k: (v if isinstance(v, int) else v) for k, v in h.items()},
            "layout": lay,
            "primary_tileset": r.prim,
            "secondary_tileset": r.sec,
            "primary_tiles_bytes": len(r.prim_tiles),
            "secondary_tiles_bytes": len(r.sec_tiles),
            "cell_0_0": {"raw": c00, "metatile": mid0,
                         "collision": (c00 >> 10) & 3, "elevation": (c00 >> 12) & 0xF},
            "cell_centre": {"raw": ccx, "metatile": ccx & 0x03FF,
                            "collision": (ccx >> 10) & 3, "elevation": (ccx >> 12) & 0xF},
            "metatile_0_0_entries": ["0x%04X" % e for e in r.metatile_entries(mid0)],
            "metatile_0_0_attr": "0x%04X" % r.attr(mid0),
            "metatile_0_0_layer_type": (r.attr(mid0) >> 12) & 0xF,
            "metatile_0_0_pixels_bgr555": ["0x%04X" % p for p in blk],
            "palette_slot_of_first_entry": (r.metatile_entries(mid0)[0] >> 12) & 0x0F,
            "palette_used": ["0x%04X" % c for c in
                             r._pal((r.metatile_entries(mid0)[0] >> 12) & 0x0F)],
        }

    jf = os.path.join(out, "render-truth.json")
    with open(jf, "w") as f:
        json.dump(truth, f, indent=1)
    print("\nwrote %s" % jf)


if __name__ == "__main__":
    main()
