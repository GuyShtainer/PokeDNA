#!/usr/bin/env python3
"""Generate the real Gen-3 BAG-screen backgrounds for pdna_bag.c — one game per
entry in GAMES (index = PkGame: 0 RS, 1 Emerald, 2 FRLG), one full 240x160
RGB15 frame per gender plus the pocket-switch animation rects, all
pre-composited at build time from the decomp art in assets/bag/<game>/.

Decomp facts per game (screen-layout rects live in source/bag_bg.h BAG_LAYOUTS,
hand-written from each game's src/item_menu.c — this script only emits pixels;
the anim geometry below MUST match that table's anim_x/anim_y/rise):

  emerald (pokeemerald graphics/bag/, src/item_menu_icons.c):
    menu.png/.bin + 32-color menu_male/menu_female.pal (2 banks);
    bag_*.png 64x384 = closed + one OPEN frame per pocket
    (Items/KeyItems/PokeBalls/TMsHMs/Berries = frames 1..5); sprite center
    (68,66) -> top-left (36,34); pocket switch: closed bag pops to y2=-5,
    falls 1 px/frame (SetBagVisualPocketId).
  rs (pokeruby graphics/interface/ + graphics/misc/, src/item_menu.c):
    bag_screen.png/.bin + 32-color bag_screen_male/_female.pal; the empty
    pocket-label pill is baked into the tilemap at tile (4,10) (the labeled
    pills live in bag_screen_labels.bin — unused here, PokeDNA draws its own
    pocket text); gendered sheets share bag.pal, SAME frame layout as Emerald
    (sBagSpriteAnimSeq1/5/2/3/4 -> sheet 1=Items 2=KeyItems 3=PokeBalls
    4=TMsHMs 5=Berries); sprite center (58,40) -> top-left (26,8); pocket
    switch: closed bag pops y -= 4, falls 1 px every 2 frames (sub_80A79EC).
    The bag_spinner.png pocket wheel is not reproduced.
  frlg (pokefirered graphics/item_menu/ + graphics/interface/, src/bag.c,
        src/item_menu_icons.c):
    bg.png (embedded 48-color pal = the 3 male banks) + bg.bin, female =
    bg_female.pal overriding bank 0 ONLY (src/item_menu.c:574); list.bin
    18x12 panel composited at tile (11,1); gendered sheets share bag.pal,
    64x256 = closed + open PokeBalls/Items/KeyItems (frames 1/2/3 —
    sAnims_Bag); sprite center (40,68) -> top-left (8,36); pocket switch:
    closed 5 frames, y2=-5, falls 1 px/frame. FRLG has only 3 bag pockets;
    PokeDNA's TMs&HMs / Berries pockets (the TM Case / Berry Pouch in-game,
    which have no bag frame) reuse the Items open frame.

Blob layout (data/bag_bg.bin), per game in PkGame order:
  2 x 240x160 RGB15 frames (male, female), then 2 x (rise+5) 64x(64+rise)
  RGB15 anim rects at screen (anim_x, anim_y): step 0..rise-1 = CLOSED bag at
  y2 = step-rise (the pop-up/fall), step rise+p = pocket p (PkPocket order
  Items/Key/Balls/TMHM/Berries) OPEN at rest.
Outputs are GIT-IGNORED (ripped art, generate-locally policy — same as
tools/gen_items.py):
  source/bag_bg_data.s   .incbin the blob into .rodata (4-byte aligned)
  source/bag_bg.c        strong bag_bg() + bag_anim() accessors (override the
                         weak NULLs in pdna_bag.c, so art-free clones build)

Run from the project root:  python3 tools/gen_bag_bg.py [--preview outdir]
"""
import os, struct, sys
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets", "bag")
OUT_BIN = os.path.join(ROOT, "data", "bag_bg.bin")
OUT_S = os.path.join(ROOT, "source", "bag_bg_data.s")
OUT_C = os.path.join(ROOT, "source", "bag_bg.c")

W, H = 240, 160                  # visible frame (30x20 of the 32x32 tilemap)
AN_W = 64                        # anim rect width = the 64x64 bag sprite

# Index = PkGame (gen3_trainer.h: PK_RS=0, PK_EMERALD=1, PK_FRLG=2).
# bag_xy = sprite top-left; anim rect top-left = (bag_x, bag_y - rise), 64 x
# (64+rise) — keep in sync with BAG_LAYOUTS in source/bag_bg.h.
# pocket_frame maps OUR PkPocket order to the gender sheet's frame number.
GAMES = [
    dict(name="rs", tileset="bag_screen.png", tilemap="bag_screen.bin",
         pal=("bag_screen_male.pal", "bag_screen_female.pal"), overlay=None,
         sheet_pal="bag.pal", pocket_frame=(1, 2, 3, 4, 5),
         bag_xy=(26, 8), rise=4),
    dict(name="emerald", tileset="menu.png", tilemap="menu.bin",
         pal=("menu_male.pal", "menu_female.pal"), overlay=None,
         sheet_pal=None, pocket_frame=(1, 2, 3, 4, 5),
         bag_xy=(36, 34), rise=5),
    dict(name="frlg", tileset="bg.png", tilemap="bg.bin",
         pal=(None, "bg_female.pal"),          # male = bg.png's embedded 48 colors
         overlay=("list.bin", 11, 1, 18, 12),  # list panel at tile (11,1)
         sheet_pal="bag.pal", pocket_frame=(2, 3, 1, 2, 2),
         bag_xy=(8, 36), rise=5),
]


def rgb15(r, g, b):
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def load_jasc_pal(path):
    """JASC-PAL -> list of RGB15 (16 colors = 1 bank, 32 = 2 banks, ...)."""
    lines = open(path).read().split()
    # header: JASC-PAL / 0100 / N, then N x "R G B" (split() flattens them)
    n = int(lines[2])
    vals = [int(v) for v in lines[3:3 + n * 3]]
    return [rgb15(vals[i * 3], vals[i * 3 + 1], vals[i * 3 + 2]) for i in range(n)]


def load_png_pal(path, n):
    """first n entries of an indexed PNG's embedded palette -> RGB15 list."""
    p = Image.open(path).getpalette()
    return [rgb15(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]) for i in range(n)]


def load_tiles(path):
    """indexed PNG -> list of 8x8 tiles as 4-bit color indices (row-major)."""
    im = Image.open(path)
    px = im.load()
    cols, rows = im.size[0] // 8, im.size[1] // 8
    return [[px[tx * 8 + x, ty * 8 + y] & 0x0F for y in range(8) for x in range(8)]
            for ty in range(rows) for tx in range(cols)]


def game_pals(g):
    """-> (male, female) RGB15 palette lists for the screen tilemap."""
    d = os.path.join(ASSETS, g["name"])
    if g["pal"][0] is None:                      # FRLG: male = embedded 48 colors,
        male = load_png_pal(os.path.join(d, g["tileset"]), 48)
        female = load_jasc_pal(os.path.join(d, g["pal"][1])) + male[16:]  # bank 0 only
        return male, female
    return (load_jasc_pal(os.path.join(d, g["pal"][0])),
            load_jasc_pal(os.path.join(d, g["pal"][1])))


def game_tilemap(g):
    """32x32 u16 tilemap, with the game's overlay tilemap composited in."""
    d = os.path.join(ASSETS, g["name"])
    tm = list(struct.unpack("<1024H", open(os.path.join(d, g["tilemap"]), "rb").read()))
    if g["overlay"]:
        fn, ox, oy, ow, oh = g["overlay"]
        ov = struct.unpack("<%dH" % (ow * oh), open(os.path.join(d, fn), "rb").read())
        for ty in range(oh):
            for tx in range(ow):
                tm[(oy + ty) * 32 + (ox + tx)] = ov[ty * ow + tx]
    return tm


def render_screen(tiles, tm, pal):
    """tilemap -> W x H RGB15 frame. Color index 0 = the backdrop (pal[0])."""
    img = [0] * (W * H)
    for ty in range(20):
        for tx in range(30):
            e = tm[ty * 32 + tx]
            idx, hf, vf, pb = e & 0x3FF, (e >> 10) & 1, (e >> 11) & 1, (e >> 12) & 0xF
            t = tiles[idx] if idx < len(tiles) else [0] * 64
            for y in range(8):
                for x in range(8):
                    sx, sy = (7 - x if hf else x), (7 - y if vf else y)
                    ci = t[sy * 8 + sx]
                    img[(ty * 8 + y) * W + (tx * 8 + x)] = pal[pb * 16 + ci] if ci else pal[0]
    return img


def sheet_frames(png, pal_path):
    """64xN bag sheet -> N/64 64x64 frames of RGB15-or-None (index 0 =
    transparent). Colors from the shared JASC pal when given (RS/FRLG),
    else the PNG's embedded palette (Emerald)."""
    im = Image.open(png)
    px = im.load()
    if pal_path:
        pal = load_jasc_pal(pal_path)
    else:
        p = im.getpalette()
        pal = [rgb15(p[i * 3], p[i * 3 + 1], p[i * 3 + 2]) for i in range(16)]
    frames = []
    for f in range(im.size[1] // 64):
        fr = []
        for y in range(64):
            for x in range(64):
                ci = px[x, f * 64 + y]
                fr.append(None if ci == 0 else pal[ci])
        frames.append(fr)
    return frames


def composite_bag(img, frame, bx, by):
    """paste a 64x64 sheet frame (None = transparent) at (bx,by)."""
    for y in range(64):
        for x in range(64):
            c = frame[y * 64 + x]
            if c is not None:
                img[(by + y) * W + (bx + x)] = c


def anim_rects(clean, sheet, g):
    """The rise+5 pre-composited 64x(64+rise) pocket-switch rects for one gender
    (see the module docstring): sheet frame over the bag-free screen patch at
    (bag_x, bag_y-rise). step 0..rise-1 = closed frame at region-row `step`
    (y2 = step - rise); step rise+p = pocket p's open frame at region-row rise."""
    bx, by, rise = g["bag_xy"][0], g["bag_xy"][1], g["rise"]
    ax, ay, ah = bx, by - rise, 64 + rise
    rects = []
    for step in range(rise + 5):
        f, dy = (0, step) if step < rise else (g["pocket_frame"][step - rise], rise)
        r = [clean[(ay + y) * W + ax + x] for y in range(ah) for x in range(AN_W)]
        fr = sheet[f]
        for y in range(64):
            for x in range(64):
                c = fr[y * 64 + x]
                if c is not None:
                    r[(dy + y) * AN_W + x] = c
        rects.append(r)
    return rects


def main():
    per_game = []                                # [(frames[2], anims[2], g)]
    for g in GAMES:
        d = os.path.join(ASSETS, g["name"])
        tiles = load_tiles(os.path.join(d, g["tileset"]))
        tm = game_tilemap(g)
        frames, anims = [], []
        for sex, pal in zip(("male", "female"), game_pals(g)):
            img = render_screen(tiles, tm, pal)
            sheet = sheet_frames(os.path.join(d, "bag_%s.png" % sex),
                                 os.path.join(d, g["sheet_pal"]) if g["sheet_pal"] else None)
            anims.append(anim_rects(img, sheet, g))  # rects need the bag-FREE screen
            composite_bag(img, sheet[0], *g["bag_xy"])  # bg keeps the closed bag baked in
            frames.append(img)
        per_game.append((frames, anims, g))

    os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
    offs = []                                    # per game: (bg_off, anim_off) in u16 units
    with open(OUT_BIN, "wb") as f:
        pos = 0
        for frames, anims, g in per_game:
            bg_off = pos
            for img in frames:
                f.write(struct.pack("<%dH" % (W * H), *img))
                pos += W * H
            anim_off = pos
            for rects in anims:
                for r in rects:
                    f.write(struct.pack("<%dH" % len(r), *r))
                    pos += len(r)
            offs.append((bg_off, anim_off))

    with open(OUT_S, "w") as f:
        f.write("/* GENERATED by tools/gen_bag_bg.py - do not edit, do not commit. */\n"
                "\t.section .rodata\n"
                "\t.align 2\n"
                "\t.global bag_bg_blob\n"
                "bag_bg_blob:\n"
                "\t.incbin \"%s\"\n" % OUT_BIN)

    with open(OUT_C, "w") as f:
        f.write("/* GENERATED by tools/gen_bag_bg.py - do not edit, do not commit. */\n"
                "#include \"bag_bg.h\"\n\n"
                "extern const uint16_t bag_bg_blob[];\n\n"
                "/* per-game (PkGame order) offsets in u16 units into the blob:\n"
                " * 2 fullscreen frames at .bg, then 2 x .steps anim rects of\n"
                " * .rect u16 each at .anim (male's rects, then female's). */\n"
                "static const struct { uint32_t bg, anim, rect; uint8_t steps; } OFF[3] = {\n")
        for (bg_off, anim_off), g in zip(offs, GAMES):
            f.write("  { %du, %du, %du, %d },   /* %s */\n"
                    % (bg_off, anim_off, AN_W * (64 + g["rise"]), g["rise"] + 5, g["name"]))
        f.write("};\n\n"
                "const uint16_t* bag_bg(int game, int female) {\n"
                "  if (game < 0 || game > 2) return 0;\n"
                "  return bag_bg_blob + OFF[game].bg + (female ? BAG_BG_W * BAG_BG_H : 0);\n"
                "}\n\n"
                "const uint16_t* bag_anim(int game, int female, int step) {\n"
                "  if (game < 0 || game > 2 || step < 0 || step >= OFF[game].steps) return 0;\n"
                "  return bag_bg_blob + OFF[game].anim\n"
                "       + ((female ? OFF[game].steps : 0) + step) * OFF[game].rect;\n"
                "}\n")

    if "--preview" in sys.argv:
        outdir = sys.argv[sys.argv.index("--preview") + 1]
        os.makedirs(outdir, exist_ok=True)

        def to_rgb(buf, w, h):
            im = Image.new("RGB", (w, h))
            sp = im.load()
            for y in range(h):
                for x in range(w):
                    c = buf[y * w + x]
                    sp[x, y] = ((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31,
                                ((c >> 10) & 31) * 255 // 31)
            return im

        for frames, anims, g in per_game:
            ah = 64 + g["rise"]
            for name, img, rects in zip(("male", "female"), frames, anims):
                to_rgb(img, W, H).save(os.path.join(outdir, "bag_%s_%s.png" % (g["name"], name)))
                strip = Image.new("RGB", ((AN_W + 2) * len(rects) - 2, ah))
                for i, r in enumerate(rects):    # contact sheet: steps left->right
                    strip.paste(to_rgb(r, AN_W, ah), (i * (AN_W + 2), 0))
                strip.save(os.path.join(outdir, "bag_anim_%s_%s.png" % (g["name"], name)))
        print("previews in", outdir)

    print("wrote %s (%d B: %s), %s, %s"
          % (OUT_BIN, os.path.getsize(OUT_BIN),
             ", ".join("%s@%d" % (g["name"], o[0] * 2) for o, g in zip(offs, GAMES)),
             OUT_S, OUT_C))


if __name__ == "__main__":
    main()
