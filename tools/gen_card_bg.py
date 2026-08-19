#!/usr/bin/env python3
"""Generate the real Gen-3 TRAINER CARD fronts AND backs for pdna_trainer.c —
one game per entry in GAMES (index = PkGame: 0 RS, 1 Emerald, 2 FRLG), one full
240x160 RGB15 frame per (face, star tier 0..4, gender): the card tileset
rendered through its bg + front (or back) tilemaps, the front additionally with
the tier's star row and the trainer photo pre-composited at build time. The
tier is palette-only (banks 0-2); female additionally swaps bank 1
(female_bg.pal = the surround) and the photo.

Card BACK (each game's back.bin): every game keeps the screen background
(bg.bin) on its own BG and swaps only the card layer front.bin -> back.bin,
with the photo/stars/badges overlay layer CLEARED — pokeruby trainer_card.c
sub_8093F48 (back.bin -> the front's VRAM slot) + TrainerCard_ClearTrainerGraphics,
pokeemerald trainer_card.c Task_DrawFlippedCardSide case 0
(FillBgTilemapBufferRect_Palette0(3,...)) + case 2 DrawCardFrontOrBack(backTilemap),
pokefirered the same flow. The back tilemaps reference pal bank 0 only (tier
colors), but the surround stays gender-swapped, so backs get the same
[tier][gender] frame set as fronts. RS's back, like its front, sits on a BG
with VOFS=-4 (TrainerCard_ResetOffsetRegisters) -> +4 px.
Also emits each game's 8 gym-badge 16x16 sprites (drawn per owned badge at
runtime) and the Hoenn-dex -> National-dex table (RS/Emerald "complete dex"
star achievement set; FRLG's dex stars are plain national ranges, no table).

Decomp facts per game (overlay text coords live in source/card_bg.h
CARD_LAYOUTS, hand-written from each game's src/trainer_card.c — this script
only emits pixels + the dex table):

  rs (pokeruby src/trainer_card.c):
    tiles.png 128x80; bg.bin/front.bin 1280 B = 32x20 u16 (stride 32!).
    BG3 = background (bg.bin, VOFS 0); BG2 = front (front.bin) and BG1 =
    stars/badges/photo overlay, both with VOFS = -4 (TrainerCard_
    ResetOffsetRegisters) -> the card content sits 4 px LOWER on screen.
    Tier pals {0..4}star.pal (48 colors -> banks 0-2, tier = star count);
    female_bg.pal -> bank 1; star.pal -> bank 4. Labels (NAME/MONEY/POKeDEX/
    PLAY TIME/IDNo./BADGES + empty-slot digits) are BAKED into front.bin.
    Stars: tile 143 pal 4 at map (15+i, 6) -> px (120+8i, 52).
    Photo: 64x64 at map (19..26, 5..12) -> px (152, 44); brendan/may.png
    (embedded pals). Badges: badges.png tiles are indirected through
    badges_map.bin (8 x 4 u16 ABSOLUTE tile ids, badge tiles based at 164).
  emerald (pokeemerald src/trainer_card.c — unchanged from the first version):
    tiles.png 128x80; bg/front.bin 1200 B = 30x20, no scroll offset. Tier pals
    green/bronze/copper/silver/gold; stars (120+8i, 56); photo (153, 40) =
    window (19,5) + Hoenn pic offset {1,0}; badges.png = 8 plain 16x16.
  frlg (pokefirered src/trainer_card.c):
    tiles.png 128x96 (192 tiles); bg/front.bin 1200 B = 30x20, no offset.
    BG2 = bg.bin surround, BG0 = front. Tier pals blue(0*)/green/bronze/
    silver/gold (sKantoTrainerCardPals); female_bg.pal -> bank 1 (case 4 of
    SetTrainerCardBgsAndPals); star.pal -> bank 4. Stars: tile 143 at
    (15, sStarYOffsets[FRLG]=7) -> px (120+8i, 56). Photo: red/leaf front
    pics + palettes at window (19,5) + Kanto pic offset {13,4} -> px (165,44).
    Badges: badges.png = 8 plain 16x16 (tiles 192+2i sequential 2x2).

Blob layout (data/card_bg.bin): per game in PkGame order, 20 frames of
240x160 RGB15, ordered [front, back][tier 0..4][male, female]. Outputs are
GIT-IGNORED
(ripped art, generate-locally policy — same as tools/gen_bag_bg.py):
  source/card_bg_data.s  .incbin the blob into .rodata (4-byte aligned)
  source/card_bg.c       strong card_bg()/card_badge16()/card_hoenn_dex()
                         (override the weak NULLs in pdna_trainer.c, so
                         art-free clones still build)

Run from the project root:  python3 tools/gen_card_bg.py [--preview outdir]
"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lzband           # paged BIOS-LZ77 art packing (see tools/lzband.py)
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets", "card")
OUT_BIN = os.path.join(ROOT, "data", "card_bg.bin")
OUT_S = os.path.join(ROOT, "source", "card_bg_data.s")
OUT_C = os.path.join(ROOT, "source", "card_bg.c")

W, H = 240, 160
STAR_TILE = 143                  # all three games' tilesets keep the star here

# Index = PkGame (gen3_trainer.h: PK_RS=0, PK_EMERALD=1, PK_FRLG=2).
# stride = tilemap row stride in entries; front_yoff = the BG VOFS shift of the
# front/overlay layers (RS -4 -> content +4 px); banks = pal banks the tilemaps
# may reference (asserted); pic_pal = separate JASC photo pals (FRLG) or None
# for the PNGs' embedded pals; badges "strip" = 8 plain 16x16 slices,
# ("map", base) = badges_map.bin absolute-tile-id indirection.
GAMES = [
    dict(name="rs", stride=32, front_yoff=4, banks={0, 1},
         tiers=("0star", "1star", "2star", "3star", "4star"),
         star_xy=(120, 52), pic=("brendan.png", "may.png"), pic_pal=None,
         pic_xy=(152, 44), badges=("map", 164)),
    dict(name="emerald", stride=30, front_yoff=0, banks={0, 1, 2},
         tiers=("green", "bronze", "copper", "silver", "gold"),
         star_xy=(120, 56), pic=("brendan.png", "may.png"), pic_pal=None,
         pic_xy=(153, 40), badges="strip"),
    dict(name="frlg", stride=30, front_yoff=0, banks={0, 1},
         tiers=("blue", "green", "bronze", "silver", "gold"),
         star_xy=(120, 56), pic=("red.png", "leaf.png"),
         pic_pal=("red.pal", "leaf.pal"), pic_xy=(165, 44), badges="strip"),
]


def load_jasc_pal(path):
    """JASC-PAL -> list of RGB15."""
    lines = open(path).read().split()
    n = int(lines[2])
    vals = [int(v) for v in lines[3:3 + n * 3]]
    return [(vals[i * 3] >> 3) | ((vals[i * 3 + 1] >> 3) << 5) | ((vals[i * 3 + 2] >> 3) << 10)
            for i in range(n)]


def load_tiles(path):
    """indexed PNG -> list of 8x8 tiles as 4-bit color indices (row-major)."""
    im = Image.open(path)
    px = im.load()
    cols, rows = im.size[0] // 8, im.size[1] // 8
    return [[px[tx * 8 + x, ty * 8 + y] & 0x0F for y in range(8) for x in range(8)]
            for ty in range(rows) for tx in range(cols)]


def render_layer(img, tiles, tm, stride, banks, yoff, bottom):
    """30x20 visible tiles of a tilemap over `img`, shifted down `yoff` px.
    `banks` = 16 palettes of 16 RGB15. The bottom layer paints color 0 as the
    backdrop (banks[0][0]); upper layers skip it (transparent)."""
    for ty in range(20):
        for tx in range(30):
            e = tm[ty * stride + tx]
            idx, hf, vf, pb = e & 0x3FF, (e >> 10) & 1, (e >> 11) & 1, (e >> 12) & 0xF
            t = tiles[idx] if idx < len(tiles) else [0] * 64
            for y in range(8):
                sy_scr = ty * 8 + y + yoff
                if sy_scr < 0 or sy_scr >= H:
                    continue
                for x in range(8):
                    sx, sy = (7 - x if hf else x), (7 - y if vf else y)
                    ci = t[sy * 8 + sx]
                    if ci:
                        img[sy_scr * W + (tx * 8 + x)] = banks[pb][ci]
                    elif bottom:
                        img[sy_scr * W + (tx * 8 + x)] = banks[0][0]


def paste_photo(img, path, dx, dy, pal):
    """paste an indexed PNG (index 0 transparent) at (dx,dy); colors from
    `pal` (RGB15 list) when given, else the PNG's embedded palette."""
    im = Image.open(path)
    px = im.load()
    p = im.getpalette()
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            ci = px[x, y]
            if ci == 0:
                continue
            if pal:
                img[(dy + y) * W + (dx + x)] = pal[ci]
            else:
                r, g, b = p[ci * 3], p[ci * 3 + 1], p[ci * 3 + 2]
                img[(dy + y) * W + (dx + x)] = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def draw_star(img, tiles, star_pal, dx, dy):
    t = tiles[STAR_TILE]
    for y in range(8):
        for x in range(8):
            ci = t[y * 8 + x]
            if ci:
                img[(dy + y) * W + (dx + x)] = star_pal[ci]


def png_tiles_rgb15(path):
    """indexed PNG -> (8x8 tiles of raw indices, palette as ui_sprite RGB15
    with 0 = transparent), tile order row-major."""
    im = Image.open(path)
    p = im.getpalette()
    pal = [0] + [0x8000 | (p[i * 3] >> 3) | ((p[i * 3 + 1] >> 3) << 5) | ((p[i * 3 + 2] >> 3) << 10)
                 for i in range(1, 16)]
    return load_tiles(path), pal


def badge_sprites(g):
    """assets/card/<game>/badges.png -> 8 x 16x16 ui_sprite blobs (0 =
    transparent). "strip": badge i = plain slice at x=16i (Emerald tile ids
    192+2i / FRLG likewise are sequential = exactly that slice). ("map", base):
    RS badges_map.bin gives 4 ABSOLUTE tile ids (TL,TR,BL,BR | pal 3 at
    runtime, tiles based at `base` = VRAM 0x1480/32) per badge."""
    d = os.path.join(ASSETS, g["name"])
    tiles, pal = png_tiles_rgb15(os.path.join(d, "badges.png"))
    if g["badges"] == "strip":
        maps = [[2 * i, 2 * i + 1, 16 + 2 * i, 17 + 2 * i] for i in range(8)]
    else:
        base = g["badges"][1]
        raw = struct.unpack("<32H", open(os.path.join(d, "badges_map.bin"), "rb").read())
        maps = [[(raw[i * 4 + k] & 0x3FF) - base for k in range(4)] for i in range(8)]
    out = []
    for m in maps:
        spr = [0] * 256
        for q, tid in enumerate(m):                    # TL TR BL BR
            t = tiles[tid] if 0 <= tid < len(tiles) else [0] * 64
            ox, oy = (q & 1) * 8, (q >> 1) * 8
            for y in range(8):
                for x in range(8):
                    spr[(oy + y) * 16 + ox + x] = pal[t[y * 8 + x]]
        out.append(spr)
    return out


def hoenn_dex_table(path):
    """pokedex.h -> the 200 National numbers of Hoenn dex 1..200 (enum order;
    Jirachi #201 / Deoxys #202 excluded, exactly HasAllHoennMons' set)."""
    nat, hoenn = [], []
    for ln in open(path):
        m = re.match(r"\s*(NATIONAL|HOENN)_DEX_([A-Z0-9_]+),\s*(//.*)?$", ln)
        if not m or m.group(2) == "NONE":
            continue
        (nat if m.group(1) == "NATIONAL" else hoenn).append(m.group(2))
    n_of = {name: i + 1 for i, name in enumerate(nat)}
    tab = [n_of[name] for name in hoenn[:200]]
    assert len(tab) == 200 and tab[0] == 252 and tab[199] == 384, "dex table sanity"
    return tab


def game_frames(g):
    """-> the 20 [front,back][tier][gender] 240x160 RGB15 frames for one game."""
    d = os.path.join(ASSETS, g["name"])
    tiles = load_tiles(os.path.join(d, "tiles.png"))
    n = g["stride"] * 20
    bg_tm = struct.unpack("<%dH" % n, open(os.path.join(d, "bg.bin"), "rb").read())
    fr_tm = struct.unpack("<%dH" % n, open(os.path.join(d, "front.bin"), "rb").read())
    bk_tm = struct.unpack("<%dH" % n, open(os.path.join(d, "back.bin"), "rb").read())
    used = {(e >> 12) & 0xF for e in bg_tm + fr_tm + bk_tm if (e & 0x3FF) or (e >> 12)}
    assert used <= g["banks"], "%s card tilemaps reference unexpected bank(s): %s" % (g["name"], used)

    female_pal = load_jasc_pal(os.path.join(d, "female_bg.pal"))
    star_pal = load_jasc_pal(os.path.join(d, "star.pal"))
    fronts, backs = [], []
    for tier in range(5):
        tp = load_jasc_pal(os.path.join(d, "%s.pal" % g["tiers"][tier]))   # 48 -> banks 0-2
        for sex in (0, 1):
            banks = [tp[0:16], tp[16:32], tp[32:48]] + [[0] * 16] * 13
            if sex:
                banks[1] = female_pal
            img = [0] * (W * H)
            render_layer(img, tiles, bg_tm, g["stride"], banks, 0, bottom=True)
            render_layer(img, tiles, fr_tm, g["stride"], banks, g["front_yoff"], bottom=False)
            paste_photo(img, os.path.join(d, g["pic"][sex]), g["pic_xy"][0], g["pic_xy"][1],
                        load_jasc_pal(os.path.join(d, g["pic_pal"][sex])) if g["pic_pal"] else None)
            for s in range(tier):                      # star row: count == tier
                draw_star(img, tiles, star_pal, g["star_xy"][0] + 8 * s, g["star_xy"][1])
            fronts.append(img)
            # back face: same surround, back.bin card layer, no photo/stars/badges
            img = [0] * (W * H)
            render_layer(img, tiles, bg_tm, g["stride"], banks, 0, bottom=True)
            render_layer(img, tiles, bk_tm, g["stride"], banks, g["front_yoff"], bottom=False)
            backs.append(img)
    return fronts + backs


def main():
    per_game = [game_frames(g) for g in GAMES]
    badges = [badge_sprites(g) for g in GAMES]
    dex = hoenn_dex_table(os.path.join(ASSETS, "emerald", "pokedex.h"))

    # 60 full-screen RGB15 frames = 4.6 MB raw, which was 37% of the whole ROM
    # and the single biggest reason the 12.5 MB image kept failing to load off
    # the SD card (projects/rom-load-lab). Flat game art, so it packs ~7x with
    # the BIOS's own LZ77 — see tools/lzband.py for the paging scheme. A frame
    # is 76800 B = 20 whole pages, so every frame is page-aligned and every
    # full-screen blit takes lzblob.c's no-copy fast path.
    raw = b"".join(struct.pack("<%dH" % (W * H), *img)
                   for frames in per_game for img in frames)
    tbl, packed_len = lzband.emit(raw, OUT_BIN, "card_bg", OUT_S, "gen_card_bg.py")
    lzband.report("card_bg", len(raw), packed_len)

    with open(OUT_C, "w") as f:
        f.write("/* GENERATED by tools/gen_card_bg.py - do not edit, do not commit. */\n"
                "#include \"card_bg.h\"\n\n"
                "/* [3 games][front,back][5 tiers][2 genders][240x160], LZ77-paged. */\n")
        f.write(tbl)
        f.write("static BgFrame frame_at(int game, int face, int tier, int female) {\n"
                "  BgFrame f = { 0, 0, CARD_BG_W };\n"
                "  if (game < 0 || game > 2) return f;\n"
                "  if (tier < 0) tier = 0;\n"
                "  if (tier > 4) tier = 4;\n"
                "  f.blob = &card_bg_blob;\n"
                "  f.off  = (uint32_t)(game * 20 + face * 10 + tier * 2 + (female ? 1 : 0))\n"
                "         * (CARD_BG_W * CARD_BG_H * 2u);\n"
                "  return f;\n"
                "}\n\n"
                "BgFrame card_bg(int game, int tier, int female) {\n"
                "  return frame_at(game, 0, tier, female);\n"
                "}\n\n"
                "BgFrame card_bg_back(int game, int tier, int female) {\n"
                "  return frame_at(game, 1, tier, female);\n"
                "}\n\n"
                "static const uint16_t badges16[3][8][256] = {\n")
        for game_badges, g in zip(badges, GAMES):
            f.write("  { /* %s */\n" % g["name"])
            for spr in game_badges:
                f.write("    {" + ",".join("0x%04X" % v for v in spr) + "},\n")
            f.write("  },\n")
        f.write("};\n\n"
                "const uint16_t* card_badge16(int game, int i) {\n"
                "  return (game >= 0 && game < 3 && i >= 0 && i < 8) ? badges16[game][i] : 0;\n"
                "}\n\n"
                "/* Hoenn dex 1..200 -> National number (HasAllHoennMons' species set) */\n"
                "static const uint16_t hoenn200[200] = {\n")
        for i in range(0, 200, 16):
            f.write("  " + ",".join("%d" % v for v in dex[i:i + 16]) + ",\n")
        f.write("};\n\n"
                "const uint16_t* card_hoenn_dex(void) { return hoenn200; }\n")

    if "--preview" in sys.argv:
        outdir = sys.argv[sys.argv.index("--preview") + 1]
        os.makedirs(outdir, exist_ok=True)
        for frames, g in zip(per_game, GAMES):
            for i, img in enumerate(frames):
                im = Image.new("RGB", (W, H))
                sp = im.load()
                for y in range(H):
                    for x in range(W):
                        c = img[y * W + x]
                        sp[x, y] = ((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31,
                                    ((c >> 10) & 31) * 255 // 31)
                j = i % 10
                im.save(os.path.join(outdir, "card_%s_%s_%s%s.png"
                                     % (g["name"], g["tiers"][j // 2], "mf"[j % 2],
                                        "_back" if i >= 10 else "")))
        print("previews in", outdir)

    print("wrote %s (%d B = 3 games x 20 x %dx%d RGB15), %s, %s"
          % (OUT_BIN, os.path.getsize(OUT_BIN), W, H, OUT_S, OUT_C))


if __name__ == "__main__":
    main()
