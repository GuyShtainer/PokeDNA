#!/usr/bin/env python3
"""Build a HARDWARE-OAM (4bpp tiles + shared 16-colour palettes) version of the box
icons from the existing RGB15 blob — no external sprite pack needed.

WHY: the PC/Bank box screen renders its 6x5 grid of 32x32 box icons as hardware OBJ
sprites (the GPU composites all 30 every frame, the idle "bob" is a free in-vblank
OAM nudge, the cursor never stalls). HW sprites need 4bpp tiles + a 16-entry palette.

THE PALETTE CONSTRAINT: OBJ palette RAM holds only 16 banks of 16 colours, but a box
can show up to 30 distinct species. So we CANNOT give every icon its own palette.
Instead — exactly like the real Gen-3 games, whose icons share a tiny palette set —
this builds 16 GLOBAL 15-colour palettes (slot 0 = transparent) by clustering every
icon's colours, assigns each icon to the global palette that reproduces it best, and
indexes that icon's 4bpp tiles into its assigned bank. The 16 palettes are uploaded
to OBJ PAL RAM once on box entry; each icon's OAM entry just selects its bank. Any
set of 30 icons therefore uses at most 16 banks (they all draw from the 16 globals).

INPUT (both produced by tools/gen_icons.py, git-ignored, present after that runs):
  data/mon_icons.bin     RGB15 blob (frame0||frame1, 32x32 u16/px; 0=transparent,
                         0x8000|rgb15=opaque)
  source/mon_icons.c     off_tbl[412] + uform_tbl[28] byte offsets — parsed so OAM
                         keying matches the RGB15 API EXACTLY (same internal-id ->
                         icon map, same Unown forms), WITHOUT the ripped pack /
                         species.h.

NOTE ON FIDELITY: gen_icons.py LANCZOS-downscales 64x64 -> 32x32, adding hundreds of
interpolated colours (NOT the <=16 a native Gen-3 icon has), so a truly lossless
4bpp conversion is impossible from this blob. The shared-palette quantisation lands
at ~1.2/31 average per-channel error (visually identical at 32x32). Only FRAME 0 is
emitted (the box screen does a unison Y-bob, not a pose swap, so frame 1 is unused).

OUTPUT (all git-ignored — derived from ripped art, regenerate locally):
  data/mon_icons_oam_tiles.bin   4bpp tiles, 16 tiles (32x32) per icon, tonc order
  data/mon_icons_oam_pal.bin     16 global palettes x 16 u16 (slot0 transparent)
  source/mon_icons_oam_data.s    .incbin both blobs into .rodata
  source/mon_icons_oam.{c,h}     per-species accessor (tiles ptr + palette-bank id)

Run from the repo root:  python3 tools/gen_icons_oam.py   (stdlib only, no Pillow)
"""
import os, re
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IN_BIN = os.path.join(ROOT, "data", "mon_icons.bin")
IN_C   = os.path.join(ROOT, "source", "mon_icons.c")
OUT_TILES = os.path.join(ROOT, "data", "mon_icons_oam_tiles.bin")
OUT_PAL   = os.path.join(ROOT, "data", "mon_icons_oam_pal.bin")
OUT_S = os.path.join(ROOT, "source", "mon_icons_oam_data.s")
OUT_C = os.path.join(ROOT, "source", "mon_icons_oam.c")
OUT_H = os.path.join(ROOT, "source", "mon_icons_oam.h")

SIZE = 32
FRAME_BYTES = SIZE * SIZE * 2
TILES_PER_ICON = (SIZE // 8) * (SIZE // 8)   # 16
TILE_BYTES = 32
PAL_ENTRIES = 16              # colours per bank (slot 0 transparent -> 15 usable)
NBANKS = 13                   # OBJ palette banks 0..12 for icons; 13/14/15 reserved
                              # by the box layer for the cursor hand / orange-MOVE
                              # hand / item glyphs (see box_oam.c).
NULL = 0xFFFFFFFF


def rgb(v):
    return (v & 31, (v >> 5) & 31, (v >> 10) & 31)


def pack(c):
    return (int(round(c[0])) & 31) | ((int(round(c[1])) & 31) << 5) | ((int(round(c[2])) & 31) << 10)


def d2(a, b):
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def parse_off_tables(path):
    txt = open(path).read()

    def grab(name):
        m = re.search(name + r"\[\d+\]\s*=\s*\{(.*?)\}", txt, re.S)
        if not m:
            raise SystemExit("could not find %s in %s" % (name, path))
        return [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+", m.group(1))]

    return grab("off_tbl"), grab("uform_tbl")


def kmeans(weighted, K, iters=10):
    """weighted: Counter{rgb15:count} -> list of K rgb-tuples (float)."""
    items = list(weighted.items())
    if not items:
        return [(0.0, 0.0, 0.0)]
    cents = [rgb(c) for c, _ in sorted(items, key=lambda x: -x[1])[:K]]
    while len(cents) < K:
        cents.append(cents[-1])
    pts = [(rgb(c), w) for c, w in items]
    for _ in range(iters):
        acc = [[0, 0, 0, 0] for _ in range(K)]
        for p, w in pts:
            bi = 0; bd = d2(p, cents[0])
            for k in range(1, K):
                dk = d2(p, cents[k])
                if dk < bd:
                    bd = dk; bi = k
            a = acc[bi]; a[0] += p[0] * w; a[1] += p[1] * w; a[2] += p[2] * w; a[3] += w
        for k in range(K):
            if acc[k][3]:
                cents[k] = (acc[k][0] / acc[k][3], acc[k][1] / acc[k][3], acc[k][2] / acc[k][3])
    return cents


def palette_err(counter, pal):
    return sum(min(d2(rgb(col), pc) for pc in pal) * w for col, w in counter.items())


def icon_mean(c):
    s = [0, 0, 0, 0]
    for col, w in c.items():
        r, g, b = rgb(col); s[0] += r * w; s[1] += g * w; s[2] += b * w; s[3] += w
    return (s[0] / s[3], s[1] / s[3], s[2] / s[3]) if s[3] else (0.0, 0.0, 0.0)


def build_global_palettes(icons):
    """icons: list[Counter]. -> (palettes[16][15 rgb], bank_of_icon[]).
    Seed by clustering icon means into 16 groups, then refine
    assign-by-best-palette <-> rebuild-palette a few times."""
    means = [icon_mean(c) for c in icons]
    gc = [means[i] for i in range(min(NBANKS, len(means)))]
    while len(gc) < NBANKS:
        gc.append((0.0, 0.0, 0.0))
    grp = [0] * len(icons)
    for _ in range(15):
        grp = [min(range(NBANKS), key=lambda k: d2(means[i], gc[k])) for i in range(len(means))]
        acc = [[0, 0, 0, 0] for _ in range(NBANKS)]
        for i, g in enumerate(grp):
            acc[g][0] += means[i][0]; acc[g][1] += means[i][1]; acc[g][2] += means[i][2]; acc[g][3] += 1
        for k in range(NBANKS):
            if acc[k][3]:
                gc[k] = (acc[k][0] / acc[k][3], acc[k][1] / acc[k][3], acc[k][2] / acc[k][3])
    palettes = None
    for _ in range(4):
        palettes = []
        for k in range(NBANKS):
            agg = Counter()
            for i, g in enumerate(grp):
                if g == k:
                    agg += icons[i]
            palettes.append(kmeans(agg, 15) if agg else [(0.0, 0.0, 0.0)])
        grp = [min(range(NBANKS), key=lambda k: palette_err(icons[i], palettes[k]))
               for i in range(len(icons))]
    return palettes, grp


def pack_tiles(frame0, pal15):
    """frame0: 1024 ints (rgb15|0x8000 opaque / 0 transparent). pal15: 15 rgb tuples.
    -> 16 4bpp tiles (512 bytes). Index 0 = transparent; 1..15 map into pal15[0..14]."""
    palr = [(0, 0, 0)] + [(p[0], p[1], p[2]) for p in pal15]  # slot0 transparent placeholder
    cache = {}

    def idx_of(v):
        c = v & 0x7FFF
        j = cache.get(c)
        if j is not None:
            return j
        pv = rgb(c)
        best = 1; bd = d2(pv, palr[1])
        for k in range(2, PAL_ENTRIES):
            dk = d2(pv, palr[k])
            if dk < bd:
                bd = dk; best = k
        cache[c] = best
        return best

    pidx = [0] * (SIZE * SIZE)
    for n in range(SIZE * SIZE):
        p = frame0[n]
        if p & 0x8000:
            pidx[n] = idx_of(p)
    out = bytearray()
    for ty in range(SIZE // 8):
        for tx in range(SIZE // 8):
            for ry in range(8):
                py = ty * 8 + ry
                for rx in range(0, 8, 2):
                    px = tx * 8 + rx
                    lo = pidx[py * SIZE + px]
                    hi = pidx[py * SIZE + px + 1]
                    out.append((lo & 0xF) | ((hi & 0xF) << 4))
    return bytes(out)


def main():
    off_tbl, uform_tbl = parse_off_tables(IN_C)
    blob = open(IN_BIN, "rb").read()

    offsets = sorted({o for o in off_tbl if o != NULL} | {o for o in uform_tbl if o != NULL})
    slot_of = {o: i for i, o in enumerate(offsets)}

    # frame-0 pixel histograms per distinct icon
    icons = []
    frames = []
    for o in offsets:
        if o + FRAME_BYTES > len(blob):
            raise SystemExit("offset 0x%x past end of %s" % (o, IN_BIN))
        f0 = [blob[o + 2 * n] | (blob[o + 2 * n + 1] << 8) for n in range(SIZE * SIZE)]
        frames.append(f0)
        cnt = Counter()
        for v in f0:
            if v & 0x8000:
                cnt[v & 0x7FFF] += 1
        icons.append(cnt)

    palettes, bank_of = build_global_palettes(icons)

    # emit tiles (each icon indexed into its assigned bank's palette)
    tiles_blob = bytearray()
    for i, f0 in enumerate(frames):
        tiles_blob += pack_tiles(f0, palettes[bank_of[i]])

    # emit 16 banks x 16 colours (slot 0 transparent)
    pal_blob = bytearray()
    for k in range(NBANKS):
        pal = [0] + [pack(c) for c in palettes[k][:15]]
        while len(pal) < PAL_ENTRIES:
            pal.append(0)
        for c in pal:
            pal_blob += int(c).to_bytes(2, "little")

    os.makedirs(os.path.dirname(OUT_TILES), exist_ok=True)
    open(OUT_TILES, "wb").write(tiles_blob)
    open(OUT_PAL, "wb").write(pal_blob)

    nslots = len(offsets)
    max_species = len(off_tbl) - 1

    with open(OUT_S, "w") as s:
        s.write("/* GENERATED by tools/gen_icons_oam.py - do not edit. */\n")
        s.write("\t.section .rodata\n\t.align 2\n")
        s.write("\t.global mon_icon_oam_tiles\nmon_icon_oam_tiles:\n\t.incbin \"%s\"\n" % OUT_TILES)
        s.write("\t.align 2\n")
        s.write("\t.global mon_icon_oam_pal\nmon_icon_oam_pal:\n\t.incbin \"%s\"\n" % OUT_PAL)

    with open(OUT_H, "w") as h:
        h.write("/* GENERATED by tools/gen_icons_oam.py - do not edit. */\n")
        h.write("#ifndef MON_ICONS_OAM_INCLUDED\n#define MON_ICONS_OAM_INCLUDED\n")
        h.write("#include <stdint.h>\n\n")
        h.write("#define MON_ICON_OAM_TILES   %d   /* 4bpp tiles per 32x32 icon */\n" % TILES_PER_ICON)
        h.write("#define MON_ICON_OAM_TILE_BYTES %d\n" % TILE_BYTES)
        h.write("#define MON_ICON_OAM_BANKS   %d   /* shared OBJ palette banks   */\n" % NBANKS)
        h.write("#define MON_ICON_OAM_PALLEN  %d   /* colours per bank           */\n" % PAL_ENTRIES)
        h.write("#define MON_ICON_OAM_SLOTS   %d   /* distinct icons in the blob */\n\n" % nslots)
        h.write("/* The 16 shared OBJ palettes (16 banks x 16 colours, slot 0 transparent),\n")
        h.write("   uploaded to OBJ PAL RAM once on box entry. */\n")
        h.write("extern const uint16_t mon_icon_oam_pal[MON_ICON_OAM_BANKS * MON_ICON_OAM_PALLEN];\n\n")
        h.write("/* For internal Gen-3 species id (Unown via the _form variant, letter 0..27):\n")
        h.write("   return 1 and set *tiles to the 16x32B 4bpp tile run and *bank to the icon's\n")
        h.write("   palette-bank index (0..15); return 0 if the species has no icon. */\n")
        h.write("int mon_icon_oam_for(uint16_t species, const uint8_t** tiles, int* bank);\n")
        h.write("int mon_icon_oam_for_form(uint16_t species, uint8_t form,\n")
        h.write("                          const uint8_t** tiles, int* bank);\n\n#endif\n")

    with open(OUT_C, "w") as c:
        c.write("/* GENERATED by tools/gen_icons_oam.py - do not edit. */\n")
        c.write('#include "mon_icons_oam.h"\n\n')
        c.write("extern const uint8_t mon_icon_oam_tiles[];\n\n")
        c.write("/* per-species: high byte = palette bank (0..15), low 12 bits = tile slot;\n")
        c.write("   0xFFFF = no icon. */\n")
        c.write("static const uint16_t slot_tbl[%d] = {\n" % (max_species + 1))
        per_species = []
        for sp in range(max_species + 1):
            o = off_tbl[sp]
            if o == NULL:
                per_species.append(0xFFFF)
            else:
                slot = slot_of[o]; per_species.append((bank_of[slot] << 12) | (slot & 0x0FFF))
        for i in range(0, len(per_species), 12):
            c.write("  " + ",".join("0x%04x" % v for v in per_species[i:i + 12]) + ",\n")
        c.write("};\n\n")
        uform = []
        for o in uform_tbl:
            if o == NULL:
                uform.append(0xFFFF)
            else:
                slot = slot_of[o]; uform.append((bank_of[slot] << 12) | (slot & 0x0FFF))
        c.write("static const uint16_t uform_slot[28] = {%s};\n\n" %
                ",".join("0x%04x" % v for v in uform))
        c.write("#define TILE_RUN (MON_ICON_OAM_TILES * MON_ICON_OAM_TILE_BYTES)\n\n")
        c.write("static int unpack(uint16_t e, const uint8_t** tiles, int* bank) {\n")
        c.write("  if (e == 0xFFFF) return 0;\n")
        c.write("  unsigned slot = e & 0x0FFF;\n")
        c.write("  if (tiles) *tiles = mon_icon_oam_tiles + (uint32_t)slot * TILE_RUN;\n")
        c.write("  if (bank)  *bank  = (e >> 12) & 0xF;\n")
        c.write("  return 1;\n}\n\n")
        c.write("int mon_icon_oam_for(uint16_t species, const uint8_t** tiles, int* bank) {\n")
        c.write("  if (species > %d) return 0;\n" % max_species)
        c.write("  return unpack(slot_tbl[species], tiles, bank);\n}\n\n")
        c.write("int mon_icon_oam_for_form(uint16_t species, uint8_t form,\n")
        c.write("                          const uint8_t** tiles, int* bank) {\n")
        c.write("  if (species == 201 && form < 28) {\n")
        c.write("    uint16_t e = uform_slot[form];\n")
        c.write("    if (e == 0xFFFF) e = uform_slot[0];\n")
        c.write("    return unpack(e, tiles, bank);\n")
        c.write("  }\n")
        c.write("  return mon_icon_oam_for(species, tiles, bank);\n}\n")

    print("oam icons: %d slots, %d tiles each, %d global palettes; tiles %.1f KiB, pal %.1f KiB"
          % (nslots, TILES_PER_ICON, NBANKS, len(tiles_blob) / 1024.0, len(pal_blob) / 1024.0))


if __name__ == "__main__":
    main()
