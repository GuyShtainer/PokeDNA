#!/usr/bin/env python3
"""Convert the Gen-3 sprite-pack box icons into a 32x32 RGB15 blob (native
box-icon size — no extra downscaling).

For each species PNG (Graphics/Pokemon/Icons/<NAME>.png, a 128x64 sheet = two
64x64 frames) take BOTH frames (left + right half), downscale each to 32x32, and
store the two consecutively so the box screen can animate the classic Gen-3 icon
bob (issue #11). Each pixel is a u16:
  0x0000          -> transparent
  0x8000 | RGB15  -> opaque
If a sheet has no second frame, frame 1 = frame 0 (a still icon).

Species are keyed by their INTERNAL Gen-3 id taken straight from the decomp
constants (SPECIES_KYOGRE=404 etc.) — the exact id the save stores; the old
national+25 shortcut mismapped the displaced legendaries (Kyogre->Registeel).

Emits (all git-ignored — ripped art, regenerate locally):
  data/mon_icons.bin          icons concatenated
  source/mon_icons_data.s     .incbin the blob into .rodata
  source/mon_icons.{c,h}      offset index + mon_icon_for()

Run from the repo root:  python3 tools/gen_icons.py   (needs Pillow)
"""
import os, re
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK = os.path.join(ROOT, "assets", "sprites", "Gen 3 Sprite Pack V1")
ICONS = os.path.join(PACK, "Graphics", "Pokemon", "Icons")
OUT_BIN = os.path.join(ROOT, "data", "mon_icons.bin")
OUT_S = os.path.join(ROOT, "source", "mon_icons_data.s")
OUT_C = os.path.join(ROOT, "source", "mon_icons.c")
OUT_H = os.path.join(ROOT, "source", "mon_icons.h")

SIZE = 32
MAX_INTERNAL = 411


def norm(s):
    return re.sub(r"[^A-Z0-9]", "", s.upper())


def species_map():
    path = os.path.join(ROOT, "reference", "pokeemerald_data", "include", "constants", "species.h")
    m = {}
    with open(path) as f:
        for line in f:
            mm = re.match(r"\s*#define\s+SPECIES_(\w+)\s+(\d+)", line)
            if mm:
                m[norm(mm.group(1))] = int(mm.group(2))
    if "NIDORANF" in m: m["NIDORANFE"] = m["NIDORANF"]
    if "NIDORANM" in m: m["NIDORANMA"] = m["NIDORANM"]
    return m


def _frame_bytes(frame):
    frame = frame.resize((SIZE, SIZE), Image.LANCZOS)
    px = frame.load()
    out = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = px[x, y]
            v = 0 if a < 128 else (0x8000 | (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10))
            out += v.to_bytes(2, "little")
    return bytes(out)


def conv(path):                              # -> frame0 || frame1 (each 32x32 RGB15)
    im = Image.open(path).convert("RGBA")
    w, h = im.size
    fw = w // 2 if w >= 64 else w            # 128x64 sheet -> two 64x64 frames
    f0 = im.crop((0, 0, fw, h))
    f1 = im.crop((fw, 0, w, h)) if w >= 64 else f0
    return _frame_bytes(f0) + _frame_bytes(f1)


def main():
    spmap = species_map()
    UNOWN_ID = spmap.get("UNOWN")
    DEOXYS_ID = spmap.get("DEOXYS")
    off = [0xFFFFFFFF] * (MAX_INTERNAL + 1)
    uform = [0xFFFFFFFF] * 28               # Unown letters A..? (forms 0..27)
    dform = [0xFFFFFFFF] * 4                # Deoxys formes 0..3 (Normal/Attack/Defense/Speed)
    blob = bytearray()
    slot = 0
    for fn in sorted(os.listdir(ICONS)):
        if not fn.lower().endswith(".png"):
            continue
        stem = fn[:-4]
        if stem == "000":
            continue
        intl = spmap.get(norm(re.sub(r"_\d+$", "", stem)))
        if intl == UNOWN_ID and UNOWN_ID is not None:        # Unown form icon (one per letter)
            mf = re.search(r"_(\d+)$", stem)
            form = int(mf.group(1)) if mf else 0
            if not (0 <= form < 28) or uform[form] != 0xFFFFFFFF:
                continue
            uform[form] = len(blob)                          # byte offset of frame 0
            blob += conv(os.path.join(ICONS, fn)); slot += 1
            if form == 0:
                off[UNOWN_ID] = uform[0]                      # letter A = default Unown icon
            continue
        if intl == DEOXYS_ID and DEOXYS_ID is not None:      # Deoxys forme icon (Normal/Attack/Defense/Speed)
            mf = re.search(r"_(\d+)$", stem)
            form = int(mf.group(1)) if mf else 0
            if not (0 <= form < 4) or dform[form] != 0xFFFFFFFF:
                continue
            dform[form] = len(blob)
            blob += conv(os.path.join(ICONS, fn)); slot += 1
            if form == 0:
                off[DEOXYS_ID] = dform[0]                     # Normal forme = default Deoxys icon
            continue
        if intl is None or intl > MAX_INTERNAL or off[intl] != 0xFFFFFFFF:
            continue
        off[intl] = len(blob)                                # byte offset of frame 0
        blob += conv(os.path.join(ICONS, fn))
        slot += 1

    # real Gen-3 EGG icon from the decomp (32x64 = two vertically-stacked 32x32 frames;
    # index-0/tRNS -> transparent via convert(RGBA)). Ripped art (blob git-ignored).
    egg_off = 0xFFFFFFFF
    EGG_ICON = os.path.join(ROOT, "daycare map", "pokeemerald", "graphics", "pokemon", "egg", "icon.png")
    if os.path.exists(EGG_ICON):
        eim = Image.open(EGG_ICON).convert("RGBA")
        f0 = eim.crop((0, 0, SIZE, SIZE)); f1 = eim.crop((0, SIZE, SIZE, 2 * SIZE))
        egg_off = len(blob); blob += _frame_bytes(f0) + _frame_bytes(f1); slot += 1

    os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
    with open(OUT_BIN, "wb") as f:
        f.write(blob)

    with open(OUT_S, "w") as s:
        s.write("/* GENERATED by tools/gen_icons.py - do not edit. */\n")
        s.write("\t.section .rodata\n\t.align 2\n")
        s.write("\t.global mon_icon_blob\nmon_icon_blob:\n\t.incbin \"%s\"\n" % OUT_BIN)

    # The header is COMMITTED now (canonical API; art-free builds rely on it
    # + art_fallbacks.c). Do not clobber it - emit only the data .c.
    print('  (header OUT_H is committed - not rewritten)')
    with open(OUT_C, "w") as c:
        c.write("/* GENERATED by tools/gen_icons.py - do not edit. */\n")
        c.write('#include "mon_icons.h"\n\n')
        c.write("extern const uint8_t mon_icon_blob[];\n\n")
        c.write("static const uint32_t off_tbl[%d] = {\n" % (MAX_INTERNAL + 1))
        for i in range(0, MAX_INTERNAL + 1, 8):
            c.write("  " + ",".join("0x%08x" % off[j] for j in range(i, min(i + 8, MAX_INTERNAL + 1))) + ",\n")
        c.write("};\n\n")
        c.write("#define FRAME_BYTES (MON_ICON_W * MON_ICON_H * 2)\n\n")
        c.write("const uint16_t* mon_icon_for_frame(uint16_t species, uint8_t frame) {\n")
        c.write("  if (species > %d) return 0;\n" % MAX_INTERNAL)
        c.write("  uint32_t o = off_tbl[species];\n")
        c.write("  if (o == 0xFFFFFFFFu) return 0;\n")
        c.write("  return (const uint16_t*)(mon_icon_blob + o + (frame & 1) * FRAME_BYTES);\n}\n\n")
        c.write("const uint16_t* mon_icon_for(uint16_t species) { return mon_icon_for_frame(species, 0); }\n\n")
        c.write("static const uint32_t uform_tbl[28] = {%s};\n" % ",".join("0x%08x" % v for v in uform))
        c.write("static const uint32_t dform_tbl[4] = {%s};\n" % ",".join("0x%08x" % v for v in dform))
        c.write("const uint16_t* mon_icon_for_form_frame(uint16_t species, uint8_t form, uint8_t frame) {\n")
        c.write("  if (species == 201 && form < 28) {\n")
        c.write("    uint32_t o = uform_tbl[form];\n")
        c.write("    if (o == 0xFFFFFFFFu) o = uform_tbl[0];\n")
        c.write("    if (o == 0xFFFFFFFFu) return 0;\n")
        c.write("    return (const uint16_t*)(mon_icon_blob + o + (frame & 1) * FRAME_BYTES);\n")
        c.write("  }\n")
        c.write("  if (species == 410 && form < 4) {   /* Deoxys internal id (nat 386) */\n")
        c.write("    uint32_t o = dform_tbl[form];\n")
        c.write("    if (o == 0xFFFFFFFFu) o = dform_tbl[0];\n")
        c.write("    if (o == 0xFFFFFFFFu) return 0;\n")
        c.write("    return (const uint16_t*)(mon_icon_blob + o + (frame & 1) * FRAME_BYTES);\n")
        c.write("  }\n")
        c.write("  return mon_icon_for_frame(species, frame);\n}\n")
        c.write("const uint16_t* mon_icon_for_form(uint16_t species, uint8_t form) {\n")
        c.write("  return mon_icon_for_form_frame(species, form, 0);\n}\n")
        c.write("static const uint32_t egg_off_tbl[1] = {0x%08x};   /* parsed by gen_icons_oam.py */\n" % egg_off)
        c.write("const uint16_t* mon_icon_egg_frame(uint8_t frame) {\n")
        c.write("  if (egg_off_tbl[0] == 0xFFFFFFFFu) return 0;\n")
        c.write("  return (const uint16_t*)(mon_icon_blob + egg_off_tbl[0] + (frame & 1) * FRAME_BYTES);\n}\n")
        c.write("const uint16_t* mon_icon_egg(void) { return mon_icon_egg_frame(0); }\n")

    print("icons: %d species x %d frames, %dx%d, %.1f KiB ROM"
          % (slot, 2, SIZE, SIZE, len(blob) / 1024.0))


if __name__ == "__main__":
    main()
