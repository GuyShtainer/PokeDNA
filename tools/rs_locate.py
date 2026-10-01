#!/usr/bin/env python3
"""Independent by-shape locator for the Ruby/Sapphire art tables (BACKLOG #293).

Scans a Gen-3 R/S dump for tables by SHAPE only (no symbol files, no code copied):
  sprites : four runs of 440 8-byte rows; sheets have u16 tag == row index at +6,
            palettes at +4 (normal tag == index, shiny tag == 500 + index); every pointer lands on an LZ10 blob (type byte 0x10).
  icons   : gMonIconTable = run of 440 4-aligned ROM pointers, each target has room
            for 1024 B; gMonIconPaletteIndices = 440 bytes all < 3 that sits right
            after it (4-aligned); the icon palette table = 3 rows {ptr -> 32 B, u16
            tag, u16 pad} with consecutive tags.
  wallpaper: rows of 16 B {tiles LZ10, u32 size, tilemap LZ10 -> 720 B, pal ptr},
            exactly 16 rows (R/S have no Walda set).
Prints every address with the evidence it matched.   usage: rs_locate.py <rom.gba>
"""
import struct, sys

BASE = 0x08000000

def u32(d, o): return struct.unpack_from('<I', d, o)[0]
def u16(d, o): return struct.unpack_from('<H', d, o)[0]

def ptr_to(d, v):
    return BASE <= v < BASE + len(d)

def lz10_size(d, v):
    if not ptr_to(d, v) or v - BASE + 4 > len(d): return 0
    o = v - BASE
    if d[o] != 0x10: return 0
    return d[o+1] | (d[o+2] << 8) | (d[o+3] << 16)

def scan_rows(d, stride, tag_off, n, want_ptr, tag_base=0):
    """Offsets (4-aligned) where n consecutive rows have tag == tag_base+index and ptr passes."""
    hits = []
    end = len(d) - n * stride
    for o in range(0, end, 4):
        if u16(d, o + tag_off) != tag_base: continue
        ok = True
        for i in range(n):
            r = o + i * stride
            if u16(d, r + tag_off) != tag_base + i or not want_ptr(u32(d, r)):
                ok = False; break
        if ok: hits.append(o)
    return hits

def locate_sprites(d):
    lz = lambda v: lz10_size(d, v) > 0
    out = {}
    sheets = scan_rows(d, 8, 6, 440, lz)
    pals = scan_rows(d, 8, 4, 440, lz)
    spals = scan_rows(d, 8, 4, 440, lz, tag_base=500)   # shiny tags are 500+index
    out['sheets'] = [BASE + o for o in sheets]
    out['pals'] = [BASE + o for o in pals]
    out['spals'] = [BASE + o for o in spals]
    return out

def locate_icons(d):
    res = []
    n = len(d)
    for o in range(0, n - 440 * 4, 4):
        # cheap first-row reject: row 0 (species 0 / "?") must be a ROM pointer
        if not ptr_to(d, u32(d, o)): continue
        ok = True
        for i in range(440):
            v = u32(d, o + i * 4)
            if not ptr_to(d, v) or (v & 3) or v - BASE + 1024 > n:
                ok = False; break
        if not ok: continue
        ids = o + 440 * 4
        idb = d[ids:ids + 440]
        if any(b >= 3 for b in idb): continue
        # palette table follows the id bytes (4-aligned), 3 rows {ptr, tag u16, pad u16}
        pt = (ids + 440 + 3) & ~3
        rows = [(u32(d, pt + 8*k), u16(d, pt + 8*k + 4), u16(d, pt + 8*k + 6)) for k in range(3)]
        ok2 = all(ptr_to(d, r[0]) and r[2] == 0 for r in rows) and \
              rows[1][1] == rows[0][1] + 1 and rows[2][1] == rows[1][1] + 1
        res.append(dict(icons=BASE + o, ids=BASE + ids, pals=BASE + pt,
                        pal_rows=rows, id_hist=[idb.count(k) for k in range(3)],
                        shape_ok=ok2,
                        distinct=len(set(u32(d, o + i*4) for i in range(440)))))
    return res

def locate_wallpaper(d):
    hits = []
    n = len(d)
    for o in range(0, n - 16 * 16, 4):
        ok = True
        for i in range(16):
            r = o + i * 16
            t, sz, m, p = u32(d, r), u32(d, r+4), u32(d, r+8), u32(d, r+12)
            if not (ptr_to(d, t) and ptr_to(d, m) and ptr_to(d, p)): ok = False; break
            if lz10_size(d, t) == 0 or lz10_size(d, m) != 720: ok = False; break
        if ok: hits.append(BASE + o)
    return hits


# ---- bag chrome (BACKLOG #313) ----------------------------------------------------------
# The R/S bag screen's assets sit in ONE contiguous run, in this order, each element starting
# at the next 4-aligned address after the previous element's end:
#   [LZ10 -> 12288 B] male bag sprite sheet   (64x384 4bpp = 6 frames x 2048 B)
#   [LZ10 -> 12288 B] female sheet
#   [LZ10 ->    32 B] the sheets' shared palette
#   [LZ10 ->  8192 B] screen tileset (256 tiles)
#   [LZ10 ->    64 B] screen palette A (2 banks)
#   [LZ10 ->    64 B] screen palette B (2 banks)
#   [RAW  ->  2048 B] screen tilemap: 32x32 u16, tile < 256, palette bank in {0,1},
#                     >= 20 distinct tiles
BAG_CHAIN = [('lz', 12288), ('lz', 12288), ('lz', 32), ('lz', 8192), ('lz', 64), ('lz', 64), ('map', 2048)]

def lz10_span(d, o, want):
    """(span, decoded) when an LZ10 stream at file offset o declares `want` bytes and decodes
    cleanly to exactly that; else None. Keeps only the LZ10 rules the BIOS uses."""
    if o + 4 > len(d) or d[o] != 0x10: return None
    if (d[o+1] | (d[o+2] << 8) | (d[o+3] << 16)) != want: return None
    out = bytearray(); p = o + 4
    while len(out) < want:
        if p >= len(d): return None
        fl = d[p]; p += 1
        for b in range(8):
            if len(out) >= want: break
            if fl & (0x80 >> b):
                if p + 2 > len(d): return None
                a, c = d[p], d[p+1]; p += 2
                disp = (((a & 15) << 8) | c) + 1
                if disp > len(out): return None
                for _ in range(min((a >> 4) + 3, want - len(out))): out.append(out[-disp])
            else:
                if p >= len(d): return None
                out.append(d[p]); p += 1
    return p - o, bytes(out)

def bag_map_ok(raw):
    w = struct.unpack('<1024H', raw)
    tiles = [x & 0x3FF for x in w]
    return max(tiles) < 256 and {x >> 12 for x in w} <= {0, 1} and len(set(tiles)) >= 20

def bag_chain_depth(d, o):
    """How many leading clauses of BAG_CHAIN hold when the chain starts at file offset o, and the
    element offsets reached."""
    at = []
    for i, (kind, want) in enumerate(BAG_CHAIN):
        if o + want > len(d): return i, at
        if kind == 'lz':
            r = lz10_span(d, o, want)
            if not r: return i, at
            at.append(o); o = (o + r[0] + 3) & ~3
        else:
            if not bag_map_ok(d[o:o + want]): return i, at
            at.append(o); o += want
    return len(BAG_CHAIN), at

def locate_bag(d):
    """Scan EVERY 4-aligned offset whose first word is an LZ10 header for 12288 B, and score the
    chain depth there; also (as the runner-up pool) every offset that starts the 4-clause screen
    sub-chain tileset/pal/pal/map. Returns (full hits, depth histogram, best runner-up)."""
    full, depth, tail = [], {}, {}
    for o in range(0, len(d) - 4, 4):
        if d[o] != 0x10: continue
        want = d[o+1] | (d[o+2] << 8) | (d[o+3] << 16)
        if want == 12288:
            n, at = bag_chain_depth(d, o)
            depth[n] = depth.get(n, 0) + 1
            if n == len(BAG_CHAIN): full.append([BASE + x for x in at])
        elif want == 8192:
            sub = BAG_CHAIN[3:]
            save = BAG_CHAIN
            n, at = bag_chain_depth_from(d, o, 3)
            tail[n] = tail.get(n, 0) + 1
    return full, depth, tail

def bag_chain_depth_from(d, o, first):
    """bag_chain_depth for the sub-chain starting at clause index `first`."""
    global BAG_CHAIN
    full = BAG_CHAIN
    try:
        BAG_CHAIN = full[first:]
        return bag_chain_depth(d, o)
    finally:
        BAG_CHAIN = full

def main():
    d = open(sys.argv[1], 'rb').read()
    print('rom', sys.argv[1], 'code', d[0xAC:0xB0].decode(), 'rev', d[0xBC])
    s = locate_sprites(d)
    print('sprite sheets (440 rows, tag==idx @+6, LZ10 ptr):', [hex(x) for x in s['sheets']])
    print('sprite pals   (440 rows, tag==idx @+4, LZ10 ptr):', [hex(x) for x in s['pals']])
    print('shiny pals    (440 rows, tag==500+idx @+4, LZ10 ptr):', [hex(x) for x in s['spals']])
    for r in locate_icons(d):
        print('icon table', hex(r['icons']), 'ids', hex(r['ids']), 'pals', hex(r['pals']),
              'pal rows', [(hex(a), hex(b), c) for a, b, c in r['pal_rows']],
              'id hist', r['id_hist'], 'distinct ptrs', r['distinct'], 'pal-shape', r['shape_ok'])
    print('wallpaper (16 B rows, tilemap LZ10==720 B):', [hex(x) for x in locate_wallpaper(d)])
    full, depth, tail = locate_bag(d)
    print('bag chain (7 clauses) full hits:', [[hex(x) for x in h] for h in full])
    print('bag chain depth histogram over every LZ10->12288 start (depth: count):', dict(sorted(depth.items())))
    print('bag screen sub-chain (tileset,palA,palB,map = 4 clauses) over every LZ10->8192 start:', dict(sorted(tail.items())))

if __name__ == '__main__':
    main()
