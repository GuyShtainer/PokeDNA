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
            16 consecutive standard rows, then the 32-entry Walda continuation.
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

if __name__ == '__main__':
    main()
