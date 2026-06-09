#!/usr/bin/env python3
"""Generate source/wallpapers.c — the 16 Gen-3 PC box wallpapers as deduplicated
8x8 RGB15 tiles + a 20x18 tilemap each, for the box screen to blit (no big EWRAM
buffer; no runtime palette/transparency logic).

Each wallpaper in the decomp is bg.png (an 8-tile interior pattern) + frame.png
(64 frame/scene tiles) + tilemap.bin (a 20x18 BG map) + 2 palettes (the PNGs'
own). We reconstruct the final 160x144 image (verified pixel-faithful against all
16 in-game wallpapers):
  base  = the bg.png pattern tiled across the area; its transparent index-0 is
          filled with the interior tone bg_pal[1] (so sparse wallpapers like
          Savanna don't show white gaps).
  over  = the wallpaper tilemap; color index 0 is TRANSPARENT (shows the base);
          tile idx < 64 -> frame tile, >= 64 -> bg tile.
  palette banks (from the game's DrawWallpaper: tilemap bank + 3, palettes loaded
          at BG bank 4/5): bank 0,1 -> frame palette, bank 2 -> bg palette.
Then we cut the result into 8x8 tiles, dedupe identical tiles, and emit per-
wallpaper unique tiles[] + map[360]. Output is git-ignored (generate-locally).

Reads reference/wallpapers/<name>/{bg.png,frame.png,tilemap.bin} (git-ignored).
Run from the repo root:  python3 tools/gen_wallpaper.py [--sheet out.png]
"""
import os, sys, struct
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RD = os.path.join(ROOT, "reference", "wallpapers")
OUT = os.path.join(ROOT, "source", "wallpapers.c")

# id order == sWallpapers[] in the decomp (forest=0 .. plain=15)
# (dir, frame.png path relative to RD). ids 0..15 = the standard wallpapers (own
# frame); ids 16..31 = the Emerald "Walda"/secret wallpapers, which share one of
# two frame tilesets (friends_frame1/2) instead of a per-wallpaper frame.
STD = ["forest", "city", "desert", "savanna", "crag", "volcano", "snow", "cave",
       "beach", "seafloor", "river", "sky", "polkadot", "pokecenter", "machine", "plain"]
WALDA = [("zigzagoon", 1), ("screen", 1), ("horizontal", 1), ("diagonal", 1),
         ("block", 1), ("ribbon", 1), ("pokecenter2", 1), ("frame", 1),
         ("blank", 1), ("circles", 1), ("azumarill", 2), ("pikachu", 2),
         ("legendary", 2), ("dusclops", 2), ("ludicolo", 2), ("whiscash", 2)]
ENTRIES = [(n, "%s/frame.png" % n) for n in STD] + \
          [(n, "friends_frame%d.png" % fr) for n, fr in WALDA]
NAMES = [e[0] for e in ENTRIES]
WP_W, WP_H = 160, 144            # 20x18 tiles


def load_tiles(path):
    """indexed PNG -> (list of 8x8 tiles as 4-bit indices, 16-color RGB15 palette)."""
    im = Image.open(path)
    px = im.load()
    cols, rows = im.size[0] // 8, im.size[1] // 8
    tiles = [[px[tx * 8 + x, ty * 8 + y] & 0x0F for y in range(8) for x in range(8)]
             for ty in range(rows) for tx in range(cols)]
    pal = im.getpalette() or []
    rgb15 = []
    for i in range(16):
        r, g, b = (pal[i * 3:i * 3 + 3] + [0, 0, 0])[:3] if i * 3 < len(pal) else (0, 0, 0)
        rgb15.append((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10))
    return tiles, rgb15, cols, rows


def assemble(name, frame_rel):
    """-> a WP_H x WP_W list of RGB15 pixels for wallpaper `name`.
    `frame_rel` is the frame.png path relative to RD (a per-wallpaper frame for the
    standard set, or the shared friends_frameN for the Walda set)."""
    d = os.path.join(RD, name)
    frame_t, frame_pal, _, _ = load_tiles(os.path.join(RD, frame_rel))
    bg_t, bg_pal, bgc, bgr = load_tiles(os.path.join(d, "bg.png"))
    tiles = frame_t + bg_t
    pals = [frame_pal, frame_pal, bg_pal]   # tilemap bank 0,1 -> frame, bank 2 -> bg
    tm = struct.unpack("<360H", open(os.path.join(d, "tilemap.bin"), "rb").read())
    fill = bg_pal[1]                        # interior tone replacing bg index-0 (white)

    img = [0] * (WP_W * WP_H)
    # base: bg pattern tiled, transparent index-0 -> interior tone
    for py in range(WP_H):
        for px_ in range(WP_W):
            bt = bg_t[((py // 8) % bgr) * bgc + ((px_ // 8) % bgc)]
            ci = bt[(py % 8) * 8 + (px_ % 8)]
            img[py * WP_W + px_] = bg_pal[ci] if ci else fill
    # tilemap overlay, color index 0 transparent
    for ty in range(18):
        for tx in range(20):
            e = tm[ty * 20 + tx]
            idx, hf, vf, pb = e & 0x3FF, (e >> 10) & 1, (e >> 11) & 1, (e >> 12) & 0xF
            t = tiles[idx] if idx < len(tiles) else [0] * 64
            pal = pals[pb] if pb < len(pals) else bg_pal
            for y in range(8):
                for x in range(8):
                    sx, sy = (7 - x if hf else x), (7 - y if vf else y)
                    ci = t[sy * 8 + sx]
                    if ci != 0:
                        img[(ty * 8 + y) * WP_W + (tx * 8 + x)] = pal[ci]
    return img


def tile_dedupe(img):
    """cut WP_W x WP_H into 8x8 RGB15 tiles; dedupe -> (unique_tiles, map[360])."""
    uniq, index, mp = [], {}, []
    for ty in range(18):
        for tx in range(20):
            tile = tuple(img[(ty * 8 + y) * WP_W + (tx * 8 + x)] for y in range(8) for x in range(8))
            if tile not in index:
                index[tile] = len(uniq)
                uniq.append(tile)
            mp.append(index[tile])
    return uniq, mp


def main():
    N = len(ENTRIES)
    sheet = None
    if "--sheet" in sys.argv:
        sheet = sys.argv[sys.argv.index("--sheet") + 1]
        contact = Image.new("RGB", (WP_W * 4, WP_H * ((N + 3) // 4)))

    data = []   # (name, uniq_tiles, map)
    for i, (name, frame_rel) in enumerate(ENTRIES):
        img = assemble(name, frame_rel)
        uniq, mp = tile_dedupe(img)
        data.append((name, uniq, mp))
        if sheet:
            sub = Image.new("RGB", (WP_W, WP_H)); sp = sub.load()
            for y in range(WP_H):
                for x in range(WP_W):
                    c = img[y * WP_W + x]
                    sp[x, y] = ((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31)
            contact.paste(sub, ((i % 4) * WP_W, (i // 4) * WP_H))

    if sheet:
        contact.save(sheet)
        print("contact sheet:", sheet)

    with open(OUT, "w") as c:
        c.write("/* GENERATED by tools/gen_wallpaper.py - do not edit. */\n")
        c.write("#include <stdint.h>\n\n")
        total = 0
        for name, uniq, mp in data:
            total += len(uniq)
            c.write("static const uint16_t wt_%s[%d*64] = {\n" % (name, len(uniq)))
            for t in uniq:
                c.write("  " + ",".join("0x%04x" % v for v in t) + ",\n")
            c.write("};\n")
            c.write("static const uint16_t wm_%s[360] = {\n" % name)
            for r in range(0, 360, 20):
                c.write("  " + ",".join(str(v) for v in mp[r:r + 20]) + ",\n")
            c.write("};\n\n")
        c.write("static const uint16_t* const s_wt[%d] = {%s};\n" % (N, ",".join("wt_" + n for n in NAMES)))
        c.write("static const uint16_t* const s_wm[%d] = {%s};\n" % (N, ",".join("wm_" + n for n in NAMES)))
        c.write("static const uint16_t s_nt[%d] = {%s};\n\n" % (N, ",".join(str(len(u)) for _, u, _ in data)))
        c.write("""const uint16_t* wallpaper_tile_data(int wp, int* ntiles){
  if (wp < 0 || wp >= %d) return 0;
  if (ntiles) *ntiles = s_nt[wp];
  return s_wt[wp];
}
const uint16_t* wallpaper_tilemap(int wp){ return (wp >= 0 && wp < %d) ? s_wm[wp] : 0; }
""" % (N, N))
    print("wallpapers.c: %d wallpapers (16 standard + %d Walda), %d unique tiles total (avg %.1f/wp)"
          % (N, N - 16, total, total / N))
    print("written:", OUT)


main()
