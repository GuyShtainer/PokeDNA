#!/usr/bin/env python3
"""Measure Gen-3 tileset sizes (compressed + decompressed), metatile tables and
metatile layer-type distribution in a real retail ROM. No invented numbers."""
import sys, struct, collections

ROM = sys.argv[1] if len(sys.argv) > 1 else "/Users/guyshtainer/Desktop/pokemon sav/POKEMON_EMER_BPEE00.gba"
d = open(ROM, "rb").read()
print("file", ROM, "size", len(d))
code = d[0xAC:0xB0].decode()
ver = d[0xBC]
print("code", code, "rev", ver)

VERS = {
    ("BPEE", 0): (0x08486578, 0x08481DD4, 34, 512, 6, "rse"),
    ("AXVE", 2): (0x083085A0, 0x08304F30, 34, 512, 6, "rse"),
    ("BPRE", 1): (0x08352718, 0x0834EBFC, 43, 640, 7, "frlg"),
}
mg, ml, ngroups, NPRIM, NPAL, fam = VERS[(code, ver)]

def off(a): return a - 0x08000000
def u32(a): return struct.unpack_from("<I", d, off(a))[0]
def s32(a): return struct.unpack_from("<i", d, off(a))[0]
def u16(a): return struct.unpack_from("<H", d, off(a))[0]

LAYOUT_SZ = 24 if fam == "rse" else 28
TS_ATTR = 0x10 if fam == "rse" else 0x14
ATTR_BYTES = 2 if fam == "rse" else 4

gptr = [u32(mg + 4 * i) for i in range(ngroups)]
gsize = []
for i in range(ngroups):
    end = gptr[i + 1] if i + 1 < ngroups else mg
    gsize.append((end - gptr[i]) // 4)
print("groups", ngroups, "maps", sum(gsize))


def lz77_measure(a):
    """Return (decompressed_size, compressed_stream_bytes_including_header)."""
    if d[off(a)] != 0x10:
        return None
    size = d[off(a) + 1] | (d[off(a) + 2] << 8) | (d[off(a) + 3] << 16)
    src = off(a) + 4
    out = 0
    while out < size:
        flags = d[src]; src += 1
        for bit in range(8):
            if out >= size:
                break
            if flags & (0x80 >> bit):
                b1 = d[src]; b2 = d[src + 1]; src += 2
                ln = (b1 >> 4) + 3
                out += min(ln, size - out)
            else:
                src += 1
                out += 1
    return size, src - off(a)


def lz77_decode(a):
    size = d[off(a) + 1] | (d[off(a) + 2] << 8) | (d[off(a) + 3] << 16)
    src = off(a) + 4
    out = bytearray()
    while len(out) < size:
        flags = d[src]; src += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                b1 = d[src]; b2 = d[src + 1]; src += 2
                ln = (b1 >> 4) + 3
                disp = (((b1 & 0xF) << 8) | b2) + 1
                for _ in range(min(ln, size - len(out))):
                    out.append(out[-disp])
            else:
                out.append(d[src]); src += 1
    return bytes(out)


tilesets = {}       # addr -> dict
layouts = {}        # layout addr -> (prim, sec, w, h, blocks)
maps = []           # (g,m,layout addr)
for g in range(ngroups):
    for m in range(gsize[g]):
        hdr = u32(gptr[g] + 4 * m)
        if hdr < 0x08000000 or off(hdr) + 0x1C > len(d):
            continue
        lay = u32(hdr)
        if lay < 0x08000000 or off(lay) + LAYOUT_SZ > len(d):
            continue
        w = s32(lay); h = s32(lay + 4)
        blocks = u32(lay + 0x0C)
        p = u32(lay + 0x10); s = u32(lay + 0x14)
        layouts[lay] = (p, s, w, h, blocks)
        maps.append((g, m, lay))
        for ts in (p, s):
            if ts and ts >= 0x08000000 and ts not in tilesets:
                tilesets[ts] = dict(
                    compressed=d[off(ts)], secondary=d[off(ts) + 1],
                    tiles=u32(ts + 4), pal=u32(ts + 8), mt=u32(ts + 0x0C),
                    attr=u32(ts + TS_ATTR))

print("distinct layouts", len(layouts), "distinct tilesets", len(tilesets))

rows = []
for ts, t in sorted(tilesets.items()):
    r = dict(addr=ts, **t)
    if t["compressed"]:
        mm = lz77_measure(t["tiles"])
        r["dec"], r["comp"] = mm if mm else (None, None)
    else:
        r["dec"], r["comp"] = None, None
    r["mtbytes"] = (t["attr"] - t["mt"]) if t["attr"] > t["mt"] else None
    rows.append(r)

ncomp = sum(1 for r in rows if r["compressed"])
print("compressed tilesets: %d of %d" % (ncomp, len(rows)))
dec = [r["dec"] for r in rows if r["dec"]]
comp = [r["comp"] for r in rows if r["comp"]]
print("DECOMPRESSED tile bytes : min %d  max %d  mean %d" % (min(dec), max(dec), sum(dec) / len(dec)))
print("COMPRESSED  tile bytes : min %d  max %d  mean %d" % (min(comp), max(comp), sum(comp) / len(comp)))
worstc = max(rows, key=lambda r: r["comp"] or 0)
print("worst compressed: tileset 0x%08X comp=%d dec=%d secondary=%d" % (worstc["addr"], worstc["comp"], worstc["dec"], worstc["secondary"]))
worstd = max(rows, key=lambda r: r["dec"] or 0)
print("worst decompressed: tileset 0x%08X comp=%d dec=%d secondary=%d" % (worstd["addr"], worstd["comp"], worstd["dec"], worstd["secondary"]))
print("dec > 16384 :", sum(1 for x in dec if x > 16384))
print("dec == 16384:", sum(1 for x in dec if x == 16384))
mtb = [r["mtbytes"] for r in rows if r["mtbytes"]]
print("metatile table bytes: min %d max %d" % (min(mtb), max(mtb)))
prim = [r for r in rows if not r["secondary"]]
sec = [r for r in rows if r["secondary"]]
print("primary sets %d (dec max %d), secondary %d (dec max %d)"
      % (len(prim), max(r["dec"] or 0 for r in prim), len(sec), max(r["dec"] or 0 for r in sec)))

# worst PAIR: primary+secondary decompressed together, per layout
pairmax = 0; pairwho = None
compmax = 0; compwho = None
for lay, (p, s, w, h, b) in layouts.items():
    dp = tilesets.get(p, {}).get("tiles")
    a = next((r["dec"] for r in rows if r["addr"] == p), 0) or 0
    bb = next((r["dec"] for r in rows if r["addr"] == s), 0) or 0
    if a + bb > pairmax:
        pairmax = a + bb; pairwho = (lay, p, s, a, bb)
    ca = next((r["comp"] for r in rows if r["addr"] == p), 0) or 0
    cb = next((r["comp"] for r in rows if r["addr"] == s), 0) or 0
    if max(ca, cb) > compmax:
        compmax = max(ca, cb); compwho = (lay, ca, cb)
print("worst prim+sec decompressed pair: %d  (layout 0x%08X prim 0x%08X=%d sec 0x%08X=%d)" % (pairmax, pairwho[0], pairwho[1], pairwho[3], pairwho[2], pairwho[4]))
print("largest single compressed stream referenced by any layout: %d" % compmax)

# ---- layer types -----------------------------------------------------------
lt_names = ["NORMAL", "COVERED", "SPLIT", "3?"]
glob = collections.Counter()
for ts, t in sorted(tilesets.items()):
    n = (t["attr"] - t["mt"]) // 16 if t["attr"] > t["mt"] else 0
    for i in range(n):
        if ATTR_BYTES == 2:
            a = u16(t["attr"] + 2 * i)
        else:
            a = u32(t["attr"] + 4 * i)
        glob[(a >> 12) & 0xF] += 1
print("layer types over all metatiles in all tilesets:", {lt_names[k] if k < 4 else k: v for k, v in sorted(glob.items())})

# per-map: how many CELLS use each layer type (this is what actually matters visually)
def layer_of(lay, mid):
    p, s, w, h, b = layouts[lay]
    if mid < NPRIM:
        t = tilesets.get(p); idx = mid
    else:
        t = tilesets.get(s); idx = mid - NPRIM
    if not t or t["attr"] <= t["mt"]:
        return None
    n = (t["attr"] - t["mt"]) // 16
    if idx >= n:
        return None
    if ATTR_BYTES == 2:
        a = u16(t["attr"] + 2 * idx)
    else:
        a = u32(t["attr"] + 4 * idx)
    return (a >> 12) & 0xF

cellcnt = collections.Counter()
maps_with_split = 0
worst_split = (0, None)
for g, m, lay in maps:
    p, s, w, h, b = layouts[lay]
    if w <= 0 or h <= 0 or w * h > 10240 or off(b) + 2 * w * h > len(d):
        continue
    cells = struct.unpack_from("<%dH" % (w * h), d, off(b))
    c = collections.Counter()
    for cell in cells:
        lt = layer_of(lay, cell & 0x3FF)
        if lt is None:
            c["?"] += 1
        else:
            c[lt] += 1
    cellcnt.update(c)
    if c.get(2, 0):
        maps_with_split += 1
        if c[2] > worst_split[0]:
            worst_split = (c[2], (g, m, w, h, sum(c.values())))
tot = sum(cellcnt.values())
print("map CELLS by layer type (all %d maps, %d cells):" % (len(maps), tot))
for k, v in sorted(cellcnt.items(), key=lambda kv: -kv[1]):
    nm = lt_names[k] if isinstance(k, int) and k < 4 else str(k)
    print("   %-8s %8d  %5.2f%%" % (nm, v, 100.0 * v / tot))
print("maps containing >=1 SPLIT cell: %d of %d ; worst map %s" % (maps_with_split, len(maps), worst_split))
