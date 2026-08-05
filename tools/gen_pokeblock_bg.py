#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""gen_pokeblock_bg.py — pre-render the retail Pokeblock Case screen.

Same shape as gen_bag_bg.py / gen_card_bg.py: read the decomp's tileset + tilemap +
palette, render the 240x160 screen ONCE on the PC, and emit it as a raw RGB15 blob the
GBA can DMA straight into VRAM. No LZ77 on our side, no palette work at runtime, no
EWRAM cost — the screen is a ROM->VRAM blit.

Simpler than the bag in two ways, both verified in the decomps:
  * NO gender axis. `grep -i gender` finds nothing in pokeruby/src/pokeblock.c or
    pokeemerald/src/pokeblock.c, and neither graphics dir carries a _male/_female pal.
    The case looks identical for Brendan and May.
  * FRLG has no Pokeblocks at all (no src/pokeblock.c, no `pokeblocks` in global.h —
    the case exists only as a dead link-transfer item). gen3_pokeblock.c already
    returns offset 0 for FRLG, and the accessor below returns NULL, so the screen keeps
    its plain list there.

RS vs Emerald ARE different and both are emitted: the tilemaps differ in 19 of 1024
entries (RS bakes a "Lv" column header into the list panel's top border; Emerald prints
it per row instead), which is 252 differing pixels. One shared frame would put Emerald's
missing header on a Ruby save, so they stay separate.

INPUTS (git-ignored — stage them with --stage, see below):
    assets/pokeblock/rs/{frame.png,menu.bin,menu.pal,device.png}
    assets/pokeblock/emerald/{frame.png,menu.bin,menu.pal,device.png}

OUTPUTS (all git-ignored):
    data/pokeblock_bg.bin        raw RGB15 blob
    source/pokeblock_bg_data.s   .incbin wrapper
    source/pokeblock_bg.c        offset table + the strong accessors

    python3 tools/gen_pokeblock_bg.py --stage      # copy the art out of the decomps
    python3 tools/gen_pokeblock_bg.py              # generate
    python3 tools/gen_pokeblock_bg.py --preview /tmp/pb   # + PNGs to eyeball
"""
import os
import shutil
import struct
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets", "pokeblock")
OUT_BIN = os.path.join(ROOT, "data", "pokeblock_bg.bin")
OUT_S = os.path.join(ROOT, "source", "pokeblock_bg_data.s")
OUT_C = os.path.join(ROOT, "source", "pokeblock_bg.c")

W, H = 240, 160                  # visible frame (30x20 of the 32x32 tilemap)

# index = PkGame (0 = RS, 1 = EMERALD, 2 = FRLG). FRLG is absent on purpose.
GAMES = [
    dict(name="rs", tileset="frame.png", tilemap="menu.bin", pal="menu.pal",
         device="device.png"),
    dict(name="emerald", tileset="frame.png", tilemap="menu.bin", pal="menu.pal",
         device="device.png"),
]

# The case sprite is drawn by the game at centre (56,64) -> top-left (24,32).
# (pokeemerald src/pokeblock.c CreatePokeblockCaseSprite(56, 64, 0); pokeruby the same.)
DEVICE_XY = (24, 32)

# Where the decomps live on this machine. --stage copies FROM here INTO assets/.
SRC = {
    "rs": (os.path.join(ROOT, "assets", "upstream", "pokeruby",
                        "graphics", "interface"),
           {"frame.png": "pokeblock_case_frame.png",
            "menu.bin": "pokeblock.bin",
            "menu.pal": "pokeblock_case_frame.pal",
            "device.png": "pokeblock_device.png"}),
    "emerald": (os.path.join(ROOT, "daycare map", "pokeemerald",
                             "graphics", "pokeblock"),
                {"frame.png": "menu.png",
                 "menu.bin": "menu.bin",
                 "menu.pal": "menu.pal",
                 "device.png": "device.png"}),
}


# ---- helpers (same contracts as gen_bag_bg.py) ----------------------------
def rgb15(r, g, b):
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def load_jasc_pal(path):
    """JASC-PAL -> list of RGB15. The case pals carry 96 colours = 6 banks."""
    lines = open(path).read().split()
    n = int(lines[2])
    vals = [int(v) for v in lines[3:3 + n * 3]]
    return [rgb15(vals[i * 3], vals[i * 3 + 1], vals[i * 3 + 2]) for i in range(n)]


def load_tiles(path):
    """indexed PNG -> list of 8x8 tiles as 4-bit colour indices (row-major)."""
    im = Image.open(path)
    px = im.load()
    cols, rows = im.size[0] // 8, im.size[1] // 8
    return [[px[tx * 8 + x, ty * 8 + y] & 0x0F for y in range(8) for x in range(8)]
            for ty in range(rows) for tx in range(cols)]


def render_screen(tiles, tm, pal):
    """tilemap -> W x H RGB15 frame. Colour index 0 = the backdrop (pal[0])."""
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


def composite_device(img, path):
    """Bake the 64x64 case sprite in at DEVICE_XY. Palette index 0 = transparent."""
    im = Image.open(path)
    pal = im.getpalette()
    px = im.load()
    ox, oy = DEVICE_XY
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            ci = px[x, y]
            if ci == 0:
                continue
            dx, dy = ox + x, oy + y
            if 0 <= dx < W and 0 <= dy < H:
                img[dy * W + dx] = rgb15(pal[ci * 3], pal[ci * 3 + 1], pal[ci * 3 + 2])
    return img


def tile_rgb(tiles, idx, pal, bank, w=8, h=8, idx2=None):
    """One (or two stacked) tiles as a flat RGB15 list, in palette `bank`."""
    out = []
    ids = [idx] if idx2 is None else [idx, idx2]
    for tid in ids:
        t = tiles[tid] if tid < len(tiles) else [0] * 64
        for y in range(8):
            for x in range(8):
                ci = t[y * 8 + x]
                out.append(pal[bank * 16 + ci] if ci else pal[0])
    return out


def stage():
    """Copy the art out of the local decomps into the git-ignored assets/ tree."""
    for name, (srcdir, files) in SRC.items():
        dst = os.path.join(ASSETS, name)
        os.makedirs(dst, exist_ok=True)
        for want, have in files.items():
            s = os.path.join(srcdir, have)
            if not os.path.exists(s):
                sys.exit("missing decomp asset: %s" % s)
            shutil.copyfile(s, os.path.join(dst, want))
            print("  %-14s <- %s" % (want, s))
    print("staged into", ASSETS)


def main():
    if "--stage" in sys.argv:
        stage()
        return

    preview = None
    if "--preview" in sys.argv:
        preview = sys.argv[sys.argv.index("--preview") + 1]
        os.makedirs(preview, exist_ok=True)

    if not os.path.isdir(ASSETS):
        sys.exit("no %s — run: python3 tools/gen_pokeblock_bg.py --stage" % ASSETS)

    blob, offs = [], []
    for g in GAMES:
        d = os.path.join(ASSETS, g["name"])
        tiles = load_tiles(os.path.join(d, g["tileset"]))
        pal = load_jasc_pal(os.path.join(d, g["pal"]))
        tm = list(struct.unpack("<1024H", open(os.path.join(d, g["tilemap"]), "rb").read()))

        frame = render_screen(tiles, tm, pal)
        frame = composite_device(frame, os.path.join(d, g["device"]))

        # The list-row highlight is tile 5 in palette banks 0/1/2 (TILE_HIGHLIGHT_NONE
        # 0x0005, _BLUE 0x1005, _RED 0x2005 in pokeemerald's pokeblock.c).
        hl = []
        for bank in (0, 1, 2):
            hl += tile_rgb(tiles, 5, pal, bank)

        # The "has this flavour" icon is tiles 0x17/0x18 stacked, in palette bank
        # 0..4 = Spicy/Dry/Sweet/Bitter/Sour ((i << 12) + 0x17).
        icons = []
        for bank in range(5):
            icons += tile_rgb(tiles, 0x17, pal, bank, idx2=0x18)

        offs.append((len(blob), len(blob) + len(frame), len(blob) + len(frame) + len(hl)))
        blob += frame + hl + icons

        if preview:
            im = Image.new("RGB", (W, H))
            im.putdata([((c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3)
                        for c in frame])
            p = os.path.join(preview, "pokeblock_%s.png" % g["name"])
            im.save(p)
            print("  preview", p)

    os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
    with open(OUT_BIN, "wb") as f:
        f.write(struct.pack("<%dH" % len(blob), *blob))
    print("%s: %d bytes" % (OUT_BIN, len(blob) * 2))

    with open(OUT_S, "w") as f:
        f.write("@ GENERATED by tools/gen_pokeblock_bg.py — do not edit, do not commit.\n"
                "    .section .rodata\n    .align 2\n"
                "    .global pokeblock_bg_blob\npokeblock_bg_blob:\n"
                '    .incbin "%s"\n' % OUT_BIN)

    with open(OUT_C, "w") as f:
        f.write('/* GENERATED by tools/gen_pokeblock_bg.py — do not edit, do not commit.\n'
                ' * Offsets are in u16 units into pokeblock_bg_blob. */\n'
                '#include "pokeblock_bg.h"\n\n'
                'extern const uint16_t pokeblock_bg_blob[];\n\n'
                'static const struct { unsigned bg, hl, icon; } OFF[%d] = {\n' % len(GAMES))
        for (a, b, c), g in zip(offs, GAMES):
            f.write("  { %8u, %8u, %8u },   /* %s */\n" % (a, b, c, g["name"]))
        f.write("};\n\n"
                "const uint16_t* pokeblock_bg(int game) {\n"
                "  if (game < 0 || game >= %d) return 0;   /* FRLG has no Pokeblocks */\n"
                "  return &pokeblock_bg_blob[OFF[game].bg];\n}\n\n"
                "const uint16_t* pokeblock_hl(int game, int state) {\n"
                "  if (game < 0 || game >= %d || state < 0 || state > 2) return 0;\n"
                "  return &pokeblock_bg_blob[OFF[game].hl + state * 64];\n}\n\n"
                "const uint16_t* pokeblock_flavor_icon(int game, int flavor) {\n"
                "  if (game < 0 || game >= %d || flavor < 0 || flavor > 4) return 0;\n"
                "  return &pokeblock_bg_blob[OFF[game].icon + flavor * 128];\n}\n"
                % (len(GAMES), len(GAMES), len(GAMES)))
    print("wrote", OUT_S, "and", OUT_C)


if __name__ == "__main__":
    main()
