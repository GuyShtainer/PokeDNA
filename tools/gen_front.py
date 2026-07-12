#!/usr/bin/env python3
"""Convert the Gen-3 sprite-pack FRONT sprites (normal + shiny) into LZ77-compressed
64x64 RGB15 blobs for the summary / box data panel.

Each 64x64 sprite (8 KiB raw) is compressed individually with devkitPro's gbalzss
(GBA BIOS LZ77, WRAM variant) and decompressed on demand at render time via
LZ77UnCompWram() into a single EWRAM scratch buffer. This keeps BOTH the normal
and shiny full-size sprites while halving the ROM (the EZ-Flash loads the image
into limited PSRAM for the OS-mode SD path, so the uncompressed 6+MB blob would
not fit). Pixel format per u16: 0x0000 transparent / 0x8000|RGB15 opaque.

Emits (all git-ignored — ripped art, regenerate locally):
  data/mon_front.bin            compressed normal sprites, concatenated (4-aligned)
  data/mon_front_shiny.bin      compressed shiny  sprites, concatenated (4-aligned)
  source/mon_front_data.s       .incbin the two blobs into .rodata
  source/mon_front.c            offset tables + mon_front_for() (decompresses)

Run from the repo root:  python3 tools/gen_front.py   (needs Pillow + gbalzss)
"""
import os, re, subprocess, tempfile
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK = os.path.join(ROOT, "assets", "sprites", "Gen 3 Sprite Pack V1")
FRONT = os.path.join(PACK, "Graphics", "Pokemon", "Front")
SHINY = os.path.join(PACK, "Graphics", "Pokemon", "Front shiny")
OUT_BIN = os.path.join(ROOT, "data", "mon_front.bin")
OUT_BIN_S = os.path.join(ROOT, "data", "mon_front_shiny.bin")
OUT_S = os.path.join(ROOT, "source", "mon_front_data.s")            # normal blob: always embedded
OUT_S_SHINY = os.path.join(ROOT, "source", "embed", "mon_front_shiny_data.s")  # shiny blob: NOR build only
OUT_C = os.path.join(ROOT, "source", "mon_front.c")
GBALZSS = "/opt/devkitpro/tools/bin/gbalzss"

SIZE = 64
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


def conv(path):
    im = Image.open(path).convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS)
    im = im.transpose(Image.FLIP_LEFT_RIGHT)        # face left, like the in-game summary
    px = im.load()
    out = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            r, g, b, a = px[x, y]
            v = 0 if a < 128 else (0x8000 | (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10))
            out += v.to_bytes(2, "little")
    return bytes(out)


def lz77(raw, tmp_in, tmp_out):
    with open(tmp_in, "wb") as f:
        f.write(raw)
    subprocess.run([GBALZSS, "e", tmp_in, tmp_out], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(tmp_out, "rb") as f:
        c = f.read()
    if len(c) % 4:                                   # pad so the next entry stays word-aligned
        c += b"\x00" * (4 - len(c) % 4)
    return c


def main():
    spmap = species_map()
    UNOWN_ID = spmap.get("UNOWN")
    DEOXYS_ID = spmap.get("DEOXYS")
    off_n = [0xFFFFFFFF] * (MAX_INTERNAL + 1)
    off_s = [0xFFFFFFFF] * (MAX_INTERNAL + 1)
    uform_n = [0xFFFFFFFF] * 28          # Unown letters A..? (forms 0..27)
    uform_s = [0xFFFFFFFF] * 28
    dform_n = [0xFFFFFFFF] * 4           # Deoxys formes 0..3 (Normal/Attack/Defense/Speed)
    dform_s = [0xFFFFFFFF] * 4
    normal = bytearray()
    shiny = bytearray()
    td = tempfile.mkdtemp()
    ti, to = os.path.join(td, "i.bin"), os.path.join(td, "o.bin")
    count = 0

    def add(fn):                          # compress one sprite (normal+shiny) -> (off_n, off_s)
        nraw = conv(os.path.join(FRONT, fn))
        spath = os.path.join(SHINY, fn)
        sraw = conv(spath) if os.path.exists(spath) else nraw
        on = len(normal); normal.extend(lz77(nraw, ti, to))
        os_ = len(shiny); shiny.extend(lz77(sraw, ti, to))
        return on, os_

    for fn in sorted(os.listdir(FRONT)):
        if not fn.lower().endswith(".png"):
            continue
        stem = fn[:-4]
        if stem == "000":
            continue
        intl = spmap.get(norm(re.sub(r"_\d+$", "", stem)))
        if intl == UNOWN_ID and UNOWN_ID is not None:        # Unown form sprite (one per letter)
            mf = re.search(r"_(\d+)$", stem)
            form = int(mf.group(1)) if mf else 0
            if not (0 <= form < 28) or uform_n[form] != 0xFFFFFFFF:
                continue
            uform_n[form], uform_s[form] = add(fn)
            if form == 0:                                    # letter A = the default Unown sprite
                off_n[UNOWN_ID], off_s[UNOWN_ID] = uform_n[0], uform_s[0]
            count += 1
            continue
        if intl == DEOXYS_ID and DEOXYS_ID is not None:      # Deoxys forme sprite (Normal/Attack/Defense/Speed)
            mf = re.search(r"_(\d+)$", stem)
            form = int(mf.group(1)) if mf else 0             # DEOXYS.png=0 Normal, _1=Attack, _2=Defense, _3=Speed
            if not (0 <= form < 4) or dform_n[form] != 0xFFFFFFFF:
                continue
            dform_n[form], dform_s[form] = add(fn)
            if form == 0:                                    # Normal forme = the default Deoxys sprite
                off_n[DEOXYS_ID], off_s[DEOXYS_ID] = dform_n[0], dform_s[0]
            count += 1
            continue
        if intl is None or intl > MAX_INTERNAL or off_n[intl] != 0xFFFFFFFF:
            continue
        off_n[intl], off_s[intl] = add(fn)
        count += 1

    # The real Gen-3 EGG front sprite (not a species) from the decomp graphics. Ripped art like
    # everything else (blob is git-ignored, regenerated locally). index-0/tRNS -> transparent
    # via convert(RGBA), so conv() handles it directly.
    egg_off = 0xFFFFFFFF
    EGG_FRONT = os.path.join(ROOT, "daycare map", "pokeemerald", "graphics", "pokemon", "egg", "front.png")
    if os.path.exists(EGG_FRONT):
        egg_off = len(normal); normal.extend(lz77(conv(EGG_FRONT), ti, to)); count += 1

    os.makedirs(os.path.dirname(OUT_BIN), exist_ok=True)
    with open(OUT_BIN, "wb") as f:
        f.write(normal)
    with open(OUT_BIN_S, "wb") as f:
        f.write(shiny)

    with open(OUT_S, "w") as s:                              # NORMAL blob: always embedded
        s.write("/* GENERATED by tools/gen_front.py - do not edit. */\n")
        s.write("\t.section .rodata\n\t.balign 4\n")          # LZ77UnCompWram needs a word-aligned src
        s.write('\t.global mon_front_blob\nmon_front_blob:\n\t.incbin "%s"\n' % OUT_BIN)
    os.makedirs(os.path.dirname(OUT_S_SHINY), exist_ok=True)  # SHINY blob: embedded ONLY in the NOR build
    with open(OUT_S_SHINY, "w") as s:                        # (source/embed/ is excluded from the SD build,
        s.write("/* GENERATED by tools/gen_front.py - NOR build only (SD build streams it). */\n")
        s.write("\t.section .rodata\n\t.balign 4\n")          #  where mon_front_shiny lives in sprites.pak)
        s.write('\t.global mon_front_shiny_blob\nmon_front_shiny_blob:\n\t.incbin "%s"\n' % OUT_BIN_S)

    def emit_tbl(c, name, off):
        c.write("static const uint32_t %s[%d] = {\n" % (name, MAX_INTERNAL + 1))
        for i in range(0, MAX_INTERNAL + 1, 8):
            c.write("  " + ",".join("0x%08x" % off[j] for j in range(i, min(i + 8, MAX_INTERNAL + 1))) + ",\n")
        c.write("};\n\n")

    with open(OUT_C, "w") as c:
        c.write("/* GENERATED by tools/gen_front.py - do not edit. */\n")
        c.write('#include "mon_front.h"\n#include <tonc.h>\n#include "sys.h"\n')
        c.write("#ifdef PDNA_STREAM_SPRITES\n"
                '#include "sprite_stream.h"   /* shiny fronts stream from /PokeDNA/sprites.pak */\n'
                '#include "sprite_pak.h"\n'
                "#else\n"
                "extern const uint8_t mon_front_shiny_blob[];\n"
                "#endif\n")
        c.write("extern const uint8_t mon_front_blob[];\n\n")
        emit_tbl(c, "off_n", off_n)
        emit_tbl(c, "off_s", off_s)
        c.write("static const uint32_t uform_n[28] = {%s};\n" % ",".join("0x%08x" % v for v in uform_n))
        c.write("static const uint32_t uform_s[28] = {%s};\n" % ",".join("0x%08x" % v for v in uform_s))
        c.write("static const uint32_t dform_n[4] = {%s};\n" % ",".join("0x%08x" % v for v in dform_n))
        c.write("static const uint32_t dform_s[4] = {%s};\n\n" % ",".join("0x%08x" % v for v in dform_s))
        c.write("/* ONE decompress buffer shared with mon_back.c (front & back never render at once) —\n"
                " * saves 8 KB EWRAM, which the SD build needs for the streaming read buffer. */\n")
        c.write("EWRAM_BSS uint16_t mon_decomp[MON_FRONT_W * MON_FRONT_H];\n\n")
        c.write("/* Fetch the sprite at blob-offset `o`. NORMAL fronts are always in ROM; SHINY fronts are\n"
                " * embedded (NOR) or streamed from sprites.pak (SD build) — one place decides which. */\n")
        c.write("static const uint16_t* front_at(uint32_t o, bool shiny) {\n")
        c.write("  if (o == 0xFFFFFFFFu) return 0;\n")
        c.write("#ifdef PDNA_STREAM_SPRITES\n")
        c.write("  if (shiny) return sprite_stream(PAK_FS_BASE + o, mon_decomp);\n")
        c.write("  LZ77UnCompWram(mon_front_blob + o, mon_decomp);\n")
        c.write("#else\n")
        c.write("  LZ77UnCompWram((shiny ? mon_front_shiny_blob : mon_front_blob) + o, mon_decomp);\n")
        c.write("#endif\n")
        c.write("  return mon_decomp;\n}\n\n")
        c.write("const uint16_t* mon_front_for(uint16_t species, bool shiny) {\n")
        c.write("  if (species > %d) return 0;\n" % MAX_INTERNAL)
        c.write("  return front_at((shiny ? off_s : off_n)[species], shiny);\n}\n\n")
        c.write("const uint16_t* mon_front_for_form(uint16_t species, bool shiny, uint8_t form) {\n")
        c.write("  if (species == 201 && form < 28) {            /* Unown letter A..? */\n")
        c.write("    uint32_t o = (shiny ? uform_s : uform_n)[form];\n")
        c.write("    if (o == 0xFFFFFFFFu) o = (shiny ? uform_s : uform_n)[0];\n")
        c.write("    return front_at(o, shiny);\n  }\n")
        c.write("  if (species == 386 && form < 4) {             /* Deoxys forme (0 Normal/1 Attack/2 Defense/3 Speed) */\n")
        c.write("    uint32_t o = (shiny ? dform_s : dform_n)[form];\n")
        c.write("    if (o == 0xFFFFFFFFu) o = (shiny ? dform_s : dform_n)[0];\n")
        c.write("    return front_at(o, shiny);\n  }\n")
        c.write("  return mon_front_for(species, shiny);\n}\n")
        c.write("const uint16_t* mon_front_egg(void) { return front_at(0x%08xu, false); }  /* egg is in the normal blob */\n" % egg_off)

    print("front sprites: %d species, normal %.2f MiB + shiny %.2f MiB = %.2f MiB ROM (LZ77 %dx%d)"
          % (count, len(normal) / 1048576.0, len(shiny) / 1048576.0,
             (len(normal) + len(shiny)) / 1048576.0, SIZE, SIZE))


if __name__ == "__main__":
    main()
