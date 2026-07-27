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

# THE 2026-07 GARBLE FIX. The game does NOT use every tile in the frame/bg PNG
# grids: the decomp build TRUNCATES each sheet with an explicit `-num_tiles N`
# (pokeemerald graphics_file_rules.mk:179-285) and then `cat`s frame+bg into one
# tileset — so a wallpaper's bg tiles start at index N (55 for forest, 45 for
# savanna, 56 for volcano ...), NOT at the PNG grid's tile count. Concatenating
# the full grids shifted every bg reference and scrambled the bodies of exactly
# the wallpapers whose grid != N (uniform-pattern bodies hid it).
#
# THE 2026-07 WALDA-LAYOUT FIX (the "ALL friends wallpapers corrupt" cart bug).
# Walda tilesets follow the SAME cat rule — friends_frameN is truncated to 57
# tiles and bg.4bpp is cat'ed DIRECTLY after it (graphics_file_rules.mk:281-334),
# so a Walda pattern's bg tiles start at index 57 like any other. There is NO
# padding to 64: what lands at tile 64 (byte 0x800) is a RUNTIME overlay —
# LoadWallpaperGfx (pokemon_storage_system.c:5404-5406) decompresses the 16x16
# (= 4-tile) icon glyph sWaldaWallpaperIcons[GetWaldaWallpaperIconId()] and
# CpuCopy32's it over tiles 64..67 only; bg tiles 68+ survive underneath. The
# old "pad the frame to 64" model shifted EVERY bg reference by 7 tiles (57..63
# drew blank padding, >=64 drew the wrong strip tile), garbling all 16 Friends
# bodies while the standard 16 stayed correct. We bake the DEFAULT glyph
# (iconId 0 = aqua — a fresh save's value; waldaPhrase.iconId isn't otherwise
# representable in a static pre-render).
FRAME_TILES = {"forest": 55, "city": 52, "savanna": 45, "crag": 49, "volcano": 56,
               "snow": 57, "cave": 55, "beach": 46, "seafloor": 54, "river": 51,
               "sky": 45, "polkadot": 54, "pokecenter": 35, "machine": 33, "plain": 18,
               "friends_frame1": 57, "friends_frame2": 57}   # desert: no rule = full grid
BG_TILES = {"savanna": 23, "beach": 23, "river": 11}         # others: full grid
WALDA_ICON_BASE = 64             # the 4 icon-glyph tiles land at tile 64 (byte 0x800)
WALDA_ICON = "icons/aqua.png"    # sWaldaWallpaperIcons[0] (default iconId)

# THE 2026-07-27 WALDA-COLOR FIX. The game does NOT show the Walda wallpapers with
# their PNG palettes as-is: LoadWallpaperGfx (pokemon_storage_system.c:5391-5397)
# copies wallpaper->palettes (both 16-color banks, 0x40 bytes) and then OVERWRITES
# entries [1..2] AND [17..18] — i.e. entries 1,2 of BOTH banks — with
# GetWaldaWallpaperColorsPtr(): the SAVE'S two chosen colors (SaveBlock1.waldaPhrase
# .colors[2] @ 0x3D70, global.h:849-857 + :1077; colors[0]=background,
# colors[1]=foreground). The pattern art is drawn in exactly those entries, so a
# static pre-render using the PNG palette paints it wrong/invisible (the 2026-07-27
# cart photo: sparse orange body, icon fragments only). Fix: emit the Walda tiles
# with two SENTINEL RGB15 values in those slots (chosen so no real tile pixel of any
# of the 32 wallpapers uses them — asserted below); the on-cart draw substitutes the
# save's actual colors AFTER its verified re-read (goldens stay computed on the
# sentinel data, so the verify semantics are untouched).
# Default colors for the --sheet preview = the game's ResetWaldaWallpaper defaults
# (pokemon_storage_system.c:9661-9669): RGB(21,25,30) bg, RGB(6,12,24) fg.
WALDA_DEFAULT_COLORS = (21 | (25 << 5) | (30 << 10), 6 | (12 << 5) | (24 << 10))


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


def load_icon_tiles(path):
    """Walda icon glyph (16x16 GRAYSCALE png; gbagfx maps gray g -> index g//17,
    e.g. aqua.png's 221/238 -> indices 13/14) -> 4 8x8 tiles of 4-bit indices."""
    im = Image.open(path)
    px = im.load()
    def idx(v):
        v = v[0] if isinstance(v, tuple) else v
        return (v // 17) & 0x0F if im.mode in ("L", "LA") else v & 0x0F
    return [[idx(px[tx * 8 + x, ty * 8 + y]) for y in range(8) for x in range(8)]
            for ty in range(2) for tx in range(2)]


def assemble(name, frame_rel, sent=None):
    """-> a WP_H x WP_W list of RGB15 pixels for wallpaper `name`.
    `frame_rel` is the frame.png path relative to RD (a per-wallpaper frame for the
    standard set, or the shared friends_frameN for the Walda set).
    `sent` (Walda only) = (bg_sentinel, fg_sentinel): palette entries 1,2 of BOTH
    banks are replaced with the sentinels, mirroring the game's runtime overwrite of
    palette entries [1..2]/[17..18] with the save's Walda colors (see the
    WALDA-COLOR FIX note above)."""
    d = os.path.join(RD, name)
    frame_t, frame_pal, _, _ = load_tiles(os.path.join(RD, frame_rel))
    bg_t, bg_pal, bgc, bgr = load_tiles(os.path.join(d, "bg.png"))
    if sent is not None:
        frame_pal = list(frame_pal); bg_pal = list(bg_pal)
        frame_pal[1] = bg_pal[1] = sent[0]     # entry 1 = waldaPhrase.colors[0] (background)
        frame_pal[2] = bg_pal[2] = sent[1]     # entry 2 = waldaPhrase.colors[1] (foreground)
    # truncate to the game's -num_tiles counts (see FRAME_TILES above); Walda pads
    # the shared frame to the fixed icon base of 64 tiles
    fkey = os.path.splitext(os.path.basename(frame_rel))[0]
    fname = fkey if fkey.startswith("friends_frame") else name
    frame_t = frame_t[:FRAME_TILES.get(fname, len(frame_t))]
    bg_t = bg_t[:BG_TILES.get(name, len(bg_t))]
    tiles = frame_t + bg_t            # the build's `cat frame bg`: bg starts at len(frame)
    if fkey.startswith("friends_frame"):
        # Walda: the game then CpuCopy32's the 4-tile icon glyph OVER tiles 64..67
        # (LoadWallpaperGfx; see the WALDA-LAYOUT FIX note) — bg tiles 68+ survive.
        icon_t = load_icon_tiles(os.path.join(RD, WALDA_ICON))
        while len(tiles) < WALDA_ICON_BASE + len(icon_t):
            tiles.append([0] * 64)
        tiles[WALDA_ICON_BASE:WALDA_ICON_BASE + len(icon_t)] = icon_t
    pals = [frame_pal, frame_pal, bg_pal]   # tilemap bank 0,1 -> frame, bank 2 -> bg
    tm = struct.unpack("<360H", open(os.path.join(d, "tilemap.bin"), "rb").read())
    fill = bg_pal[1]                        # interior tone replacing bg index-0 (white)

    img = [fill] * (WP_W * WP_H)
    # base layer differs by wallpaper kind:
    #  - standard: bg.png is a small (4x2) repeating SCENERY pattern, tiled across
    #    the area; the tilemap overlays frame tiles (index-0 transparent shows it).
    #  - Walda/secret: bg.png is a 22x1 STRIP of icon/pattern tiles that the tilemap
    #    references directly at idx >= 64 — it is NOT a tileable background. So the
    #    interior is just the solid fill; the tilemap draws frame + icon over it.
    #    (Tiling the strip here is what produced the garbled bands.)
    if not frame_rel.startswith("friends_frame"):
        for py in range(WP_H):
            for px_ in range(WP_W):
                bi = ((py // 8) % bgr) * bgc + ((px_ // 8) % bgc)
                if bi >= len(bg_t): continue      # truncated padding tile: keep the fill
                ci = bg_t[bi][(py % 8) * 8 + (px_ % 8)]
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


def pick_sentinels(used):
    """Two RGB15 values used by NO real tile pixel of ANY wallpaper (so the on-cart
    substitution can never touch a legitimate pixel): the first two free values
    scanning up from 1 (never 0)."""
    picked = []
    for v in range(1, 0x8000):
        if v not in used:
            picked.append(v)
            if len(picked) == 2:
                return tuple(picked)
    raise AssertionError("no free RGB15 values for the Walda sentinels")


def main():
    N = len(ENTRIES)
    sheet = None
    if "--sheet" in sys.argv:
        sheet = sys.argv[sys.argv.index("--sheet") + 1]
        contact = Image.new("RGB", (WP_W * 4, WP_H * ((N + 3) // 4)))

    # pass 1: assemble everything WITHOUT sentinels to learn every RGB15 value any
    # tile pixel can take, then pick two sentinel values outside that set.
    imgs = [assemble(name, frame_rel) for name, frame_rel in ENTRIES]
    used = set()
    for img in imgs:
        used.update(img)
    SENT = pick_sentinels(used)

    # pass 2: re-assemble the 16 Walda wallpapers with the sentinel palette slots.
    # The 16 standard wallpapers keep their pass-1 pixels (byte-identical output).
    for i, (name, frame_rel) in enumerate(ENTRIES):
        if i >= 16:
            imgs[i] = assemble(name, frame_rel, sent=SENT)
            assert all(v in used or v in SENT for v in imgs[i]), \
                "walda %s produced pixels outside used+sentinels" % name

    data = []   # (name, uniq_tiles, map)
    for i, (name, frame_rel) in enumerate(ENTRIES):
        img = imgs[i]
        uniq, mp = tile_dedupe(img)
        data.append((name, uniq, mp))
        if sheet:
            # preview substitutes the sentinels with the game's DEFAULT Walda colors
            # (what a fresh Emerald save shows) so the patterns are visible.
            subst = {SENT[0]: WALDA_DEFAULT_COLORS[0], SENT[1]: WALDA_DEFAULT_COLORS[1]}
            sub = Image.new("RGB", (WP_W, WP_H)); sp = sub.load()
            for y in range(WP_H):
                for x in range(WP_W):
                    c = subst.get(img[y * WP_W + x], img[y * WP_W + x])
                    sp[x, y] = ((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31)
            contact.paste(sub, ((i % 4) * WP_W, (i // 4) * WP_H))

    # the sentinels must appear ONLY in Walda tiles (never in the standard 16), and
    # every Walda wallpaper must actually contain them (a pattern-less emit would
    # mean the palette patch missed the drawn entries).
    for i, (name, uniq, mp) in enumerate(data):
        hit = any(v in SENT for t in uniq for v in t)
        if i < 16:
            assert not hit, "sentinel leaked into standard wallpaper %s" % name
        else:
            assert hit, "walda %s emitted no sentinel pixels (palette patch missed)" % name

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

        # build-time FNV-1a goldens over each map + tile array (u16 stream), so the
        # on-cart draw can detect DETERMINISTIC mis-reads (a repeatable wrong read
        # passes any copy+re-read verify; only a golden catches it).
        def fnv16(vals):
            s = 2166136261
            for v in vals:
                s = ((s ^ v) * 16777619) & 0xFFFFFFFF
            return s
        msums = [fnv16(mp) for _, _, mp in data]
        tsums = [fnv16([px for t in uniq for px in t]) for _, uniq, _ in data]
        c.write("static const uint32_t s_msum[%d] = {%s};\n" % (N, ",".join("0x%08xu" % v for v in msums)))
        c.write("static const uint32_t s_tsum[%d] = {%s};\n\n" % (N, ",".join("0x%08xu" % v for v in tsums)))
        # the two Walda sentinel colors: the draw path replaces them with the save's
        # waldaPhrase.colors AFTER its verified staging (see the WALDA-COLOR FIX note)
        c.write("static const uint16_t s_walda_sent[2] = {0x%04x,0x%04x};\n\n" % SENT)
        c.write("""const uint16_t* wallpaper_tile_data(int wp, int* ntiles){
  if (wp < 0 || wp >= %d) return 0;
  if (ntiles) *ntiles = s_nt[wp];
  return s_wt[wp];
}
const uint16_t* wallpaper_tilemap(int wp){ return (wp >= 0 && wp < %d) ? s_wm[wp] : 0; }
uint32_t wallpaper_map_sum(int wp){ return (wp >= 0 && wp < %d) ? s_msum[wp] : 0; }
uint32_t wallpaper_tile_sum(int wp){ return (wp >= 0 && wp < %d) ? s_tsum[wp] : 0; }
const uint16_t* wallpaper_walda_sentinels(void){ return s_walda_sent; }
""" % (N, N, N, N))
    print("wallpapers.c: %d wallpapers (16 standard + %d Walda), %d unique tiles total (avg %.1f/wp)"
          % (N, N - 16, total, total / N))
    print("written:", OUT)


main()
