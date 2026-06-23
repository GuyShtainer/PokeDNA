#!/usr/bin/env python3
"""Generate source/daycare_bg_data.h — the Day-Care yard background, drawn PROCEDURALLY
at NATIVE 240x112 resolution (no photo downscale, so no aliasing/corruption) and in a
clean-room style (our own tile art, NOT ripped decomp graphics — keeps the repo
IP-clean per docs/kb/licensing.md).

Terrain regions are positioned so the boarding-mon placement spots in source/pdna_main.c
(DC_REGION[]) land on the matching ground:
  LAVA  pool   top-left      WATER pond  top-centre
  ROCK  hill   bottom-left   GRASS open  centre
  HOUSE        bottom-right  POWER line  top-right
A tree border + fence frame the yard; flowers/rocks scatter on the grass.

Output is git-ignored (generate-locally), same contract + blit path as the old
gen_daycare_bg.py (dc_scene() does one dma3_cpy of daycare_bg[]):
  static const unsigned short __attribute__((aligned(4))) daycare_bg[240*112];

Run from the project root:  python3 tools/gen_daycare_map.py  [--png out.png]
"""
import os, sys, math, random
from PIL import Image, ImageDraw

W, H = 240, 112
random.seed(0xDADCA12E)                         # deterministic output

def C(r, g, b): return (max(0, min(255, int(r))), max(0, min(255, int(g))), max(0, min(255, int(b))))

# ---- palettes (clean-room) ----
GRASS   = [C(96,168,88), C(112,184,104), C(80,150,76)]   # base / light / dark
TREE    = [C(40,108,56), C(58,132,72), C(28,84,44)]      # canopy mid / light / dark+trunk
WATER   = [C(72,140,216), C(120,180,236), C(48,110,190)] # mid / ripple / deep
LAVA    = [C(232,96,24), C(255,176,40), C(150,40,12)]    # mid / hot / crust
ROCK    = [C(150,118,96), C(180,150,128), C(110,84,66)]  # mid / light / shadow
DIRT    = C(176,140,96)
FENCE   = [C(224,220,200), C(150,140,120)]
ROOF    = [C(196,60,48), C(150,36,30)]
WALL    = [C(214,182,130), C(150,110,70)]
FLOWERS = [C(232,72,72), C(248,216,72), C(236,236,248)]

img = Image.new("RGB", (W, H), GRASS[0])
d = ImageDraw.Draw(img)

def noise(x, y):                                # cheap stable hash noise 0..1
    n = (x * 374761393 + y * 668265263) & 0xFFFFFFFF
    n = (n ^ (n >> 13)) * 1274126177 & 0xFFFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0

# ---- grass field with a soft 2-tone texture ----
for y in range(H):
    for x in range(W):
        v = noise(x // 2, y // 2)
        img.putpixel((x, y), GRASS[1] if v > 0.80 else GRASS[2] if v < 0.18 else GRASS[0])

def blob(cx, cy, rx, ry, pal, jag=2.0, crust=None):
    """irregular filled ellipse with a 2-tone interior + optional rim colour."""
    for y in range(int(cy - ry - jag), int(cy + ry + jag)):
        if y < 0 or y >= H: continue
        for x in range(int(cx - rx - jag), int(cx + rx + jag)):
            if x < 0 or x >= W: continue
            wob = (noise(x // 3, y // 3) - 0.5) * jag * 2
            dd = ((x - cx) / (rx + wob)) ** 2 + ((y - cy) / (ry + wob)) ** 2
            if dd <= 1.0:
                t = noise(x, y)
                if crust is not None and dd > 0.78:
                    img.putpixel((x, y), crust)
                else:
                    img.putpixel((x, y), pal[1] if t > 0.72 else pal[2] if t < 0.22 else pal[0])

# ---- WATER pond (top-centre) ----
blob(135, 38, 34, 18, WATER, jag=3, crust=WATER[2])
for _ in range(60):                              # ripples
    x, y = random.randint(105, 165), random.randint(24, 52)
    if ((x-135)/34)**2 + ((y-38)/18)**2 < 0.7:
        d.line([(x, y), (x+random.randint(2,5), y)], fill=WATER[1])

# ---- LAVA pool (top-left) ----
blob(60, 40, 40, 22, LAVA, jag=3, crust=LAVA[2])
for _ in range(40):                              # bright bubbles + dark rock
    x, y = random.randint(24, 96), random.randint(22, 56)
    if ((x-60)/40)**2 + ((y-40)/22)**2 < 0.65:
        d.ellipse([x, y, x+random.randint(1,3), y+random.randint(1,3)],
                  fill=LAVA[1] if noise(x,y) > 0.5 else LAVA[2])

# ---- ROCK hill (bottom-left) ----
blob(48, 96, 46, 22, ROCK, jag=3, crust=ROCK[2])
for _ in range(40):                              # facets / shadow
    x, y = random.randint(6, 92), random.randint(78, 111)
    if ((x-48)/46)**2 + ((y-96)/22)**2 < 0.8:
        d.line([(x, y), (x, y+random.randint(2,5))], fill=ROCK[2] if noise(x,y) > 0.5 else ROCK[1])

# ---- DAY-CARE house (bottom-right) ----
hx, hy, hw = 168, 56, 60
d.polygon([(hx-4, hy+14), (hx+hw//2, hy-2), (hx+hw+4, hy+14)], fill=ROOF[0])     # roof
d.line([(hx-4, hy+14), (hx+hw//2, hy-2)], fill=ROOF[1]); d.line([(hx+hw//2, hy-2), (hx+hw+4, hy+14)], fill=ROOF[1])
d.rectangle([hx, hy+14, hx+hw, hy+50], fill=WALL[0], outline=WALL[1])           # wall
d.rectangle([hx+hw//2-8, hy+30, hx+hw//2+8, hy+50], fill=C(120,80,48), outline=C(80,52,30))  # door
d.ellipse([hx+hw//2+3, hy+39, hx+hw//2+6, hy+42], fill=C(248,224,96))           # knob
d.rectangle([hx+8, hy+20, hx+22, hy+34], fill=C(150,210,240), outline=C(80,52,30))           # window
d.line([(hx+15, hy+20), (hx+15, hy+34)], fill=C(80,52,30)); d.line([(hx+8, hy+27), (hx+22, hy+27)], fill=C(80,52,30))
d.rectangle([hx+8, hy+38, hx+hw//2-12, hy+48], fill=C(236,220,150), outline=C(110,80,40))    # sign

# ---- POWER line (top-right) ----
for px in (196, 226):
    d.rectangle([px, 14, px+2, 40], fill=C(120,96,72))
    d.line([(px-6, 18), (px+8, 18)], fill=C(90,70,52))
d.line([(196, 16), (226, 16)], fill=C(40,40,40)); d.line([(196, 20), (226, 20)], fill=C(40,40,40))

# ---- tree border (top + sides) + bottom fence ----
def tree(cx, cy):
    d.ellipse([cx-7, cy-7, cx+7, cy+6], fill=TREE[0])
    d.ellipse([cx-7, cy-8, cx+4, cy+1], fill=TREE[1])
    d.rectangle([cx-1, cy+5, cx+1, cy+9], fill=TREE[2])
for x in range(8, W, 18): tree(x, 7)             # top row
for y in range(24, H-8, 20):                     # left + right columns
    tree(8, y); tree(W-9, y)
fy = H - 6
d.line([(20, fy), (160, fy)], fill=FENCE[1]); d.line([(20, fy-3), (160, fy-3)], fill=FENCE[0])
for x in range(20, 160, 12): d.rectangle([x, fy-6, x+2, fy+3], fill=FENCE[0])

# ---- flowers + pebbles on open grass ----
for _ in range(40):
    x, y = random.randint(96, 160), random.randint(58, 104)
    img.putpixel((x, y), random.choice(FLOWERS));
    if x+1 < W: img.putpixel((x+1, y), GRASS[1])

# ---- emit ----
def to_bgr555(r, g, b): return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)
px = img.load()
vals = [to_bgr555(*px[x, y]) for y in range(H) for x in range(W)]
out = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "source", "daycare_bg_data.h"))
with open(out, "w") as f:
    f.write("/* GENERATED by tools/gen_daycare_map.py - do not edit. GIT-IGNORED. */\n")
    f.write("#ifndef DAYCARE_BG_DATA_H\n#define DAYCARE_BG_DATA_H\n")
    f.write("#define DAYCARE_BG_W %d\n#define DAYCARE_BG_H %d\n" % (W, H))
    f.write("static const unsigned short __attribute__((aligned(4)))\n  daycare_bg[DAYCARE_BG_W*DAYCARE_BG_H] = {\n")
    for i in range(0, len(vals), 16):
        f.write("  " + ",".join("0x%04x" % v for v in vals[i:i+16]) + ",\n")
    f.write("};\n#endif\n")
print("wrote %s (%dx%d)" % (out, W, H))
if "--png" in sys.argv:
    img.resize((W*3, H*3), Image.NEAREST).save(sys.argv[sys.argv.index("--png")+1])
    print("preview written")
