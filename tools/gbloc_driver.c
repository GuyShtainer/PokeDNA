/* gbloc_driver.c -- BACKLOG #68b. Computes RomGbSpriteLoc / RomGbIconLoc / RomGbUiLoc
 * records for ONE Game Boy ROM using the EXACT shipped C locators
 * (source/rom_gbsprite.c, source/rom_gbicon.c, source/rom_gbui.c), never a
 * re-implementation -- the whole point is that the fused record is what the GBA
 * build's own rom_gb*_open_loc() would have produced, not a second opinion. Compiled
 * on the fly by tools/fuse_gb.py, same posture as tools/gbui_dump.py's own driver.
 *
 * WHY: gb_art_source.c's PDNA_DELTA half (BACKLOG #62) keeps its locator cache only
 * in EWRAM for the session, so every fresh boot re-scans the whole fused ROM on the
 * first art fetch (~258 s emulated for Gen 1, ~500 s for Gen 2 -- two scans, sprite
 * portrait + menu icon). This driver runs those same scans ONCE, on the PC, at fuse
 * time, and writes their result so tools/fuse_gb.py can embed it next to the ROM;
 * source/gb_art_source.c seeds its EWRAM cache from that fused record and skips the
 * cold-start scan (rom_gb*_open_loc() STILL re-validates it before trusting it).
 *
 * Writes, back to back into one output file, one payload per locator this ROM
 * satisfies (sprite always if it opens as a GB ROM at all; icon only if the ROM is
 * Gen 2; ui if it satisfies rom_gbui's own requirements):
 *
 *   magic[8]    "PDNALOC1"
 *   kind        u8   1=sprite 2=icon 3=ui
 *   gen         u8   1 or 2
 *   rec_size    u16  little-endian, sizeof the record that follows
 *   id_hash     u32  little-endian, the locator's own id_hash (redundant cross-check)
 *   rom_size    u32  little-endian, this ROM's file size in bytes
 *   record      rec_size bytes -- RomGbSpriteLoc / RomGbIconLoc / RomGbUiLoc, written
 *               via a plain fwrite(&loc, sizeof loc, 1, f): the exact bytes the
 *               shipped C struct lays out on THIS build host. Both this host
 *               (x86-64/arm64 macOS, System V / Apple ABI) and the GBA target
 *               (ARM7TDMI, gcc -mthumb, EABI) use natural alignment with no
 *               bitfields and no member wider than uint32_t in any of the three
 *               structs, so the byte layout is identical on both -- the
 *               _Static_assert below is a real build-time check that the struct
 *               sizes are what this file assumes, not merely a comment saying so.
 *
 * Usage: gbloc_driver <rom-path> <out-path>
 * Exit 0 if at least one locator was written; 1 if this file does not open as any
 * kind of recognized Game Boy ROM at all (fuse_gb.py's own validate_payload() has
 * already refused a non-.gb/.gbc file before this driver ever runs, so that case is
 * "recognized extension, but the shipped locators still can't place its tables" --
 * fail closed exactly like the GBA build would: no LOC entries fused, the delta
 * build scans as it does today).
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rom_gbsprite.h"
#include "rom_gbicon.h"
#include "rom_gbui.h"

_Static_assert(sizeof(RomGbSpriteLoc) <= 0xFFFFu, "RomGbSpriteLoc must fit a u16 rec_size");
_Static_assert(sizeof(RomGbIconLoc)   <= 0xFFFFu, "RomGbIconLoc must fit a u16 rec_size");
_Static_assert(sizeof(RomGbUiLoc)     <= 0xFFFFu, "RomGbUiLoc must fit a u16 rec_size");

#define LOC_KIND_SPRITE 1
#define LOC_KIND_ICON   2
#define LOC_KIND_UI     3
#define LOC_SCRATCH_LEN 4096u   /* covers ROM_GBSPRITE_SCRATCH_MIN / ROM_GBICON_SCRATCH_MIN /
                                 * ROM_GBUI_SCRATCH_MIN (all 2048) with margin to spare      */

static FILE* g_f;

static bool rd(void* ctx, uint32_t off, void* dst, uint32_t len) {
  (void)ctx;
  if (fseek(g_f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, g_f) == len;
}

static uint32_t fsize(FILE* f) {
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  return (uint32_t)n;
}

static void write_payload(FILE* out, uint8_t kind, uint8_t gen, uint32_t id_hash,
                          uint32_t rom_size, const void* rec, uint16_t rec_size) {
  fwrite("PDNALOC1", 1, 8, out);
  fwrite(&kind, 1, 1, out);
  fwrite(&gen, 1, 1, out);
  fwrite(&rec_size, sizeof rec_size, 1, out);
  fwrite(&id_hash, sizeof id_hash, 1, out);
  fwrite(&rom_size, sizeof rom_size, 1, out);
  fwrite(rec, 1, rec_size, out);
}

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: %s <rom-path> <out-path>\n", argv[0]);
    return 2;
  }
  g_f = fopen(argv[1], "rb");
  if (!g_f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
  uint32_t size = fsize(g_f);

  FILE* out = fopen(argv[2], "wb");
  if (!out) { fprintf(stderr, "cannot open %s for writing\n", argv[2]); fclose(g_f); return 2; }

  static uint8_t scratch[LOC_SCRATCH_LEN];
  int wrote = 0;

  RomGbSprite gs;
  if (rom_gbsprite_open(&gs, rd, NULL, size, scratch, sizeof scratch, GB_ROM_NONE)) {
    RomGbSpriteLoc loc;
    rom_gbsprite_save_loc(&gs, &loc);
    write_payload(out, LOC_KIND_SPRITE, (uint8_t)gs.gen, gs.id_hash, size, &loc,
                 (uint16_t)sizeof loc);
    wrote++;
    printf("sprite\tgen=%d\tid_hash=0x%08X\trec_size=%zu\n", gs.gen, gs.id_hash, sizeof loc);

    if (gs.gen == GB_ROM_GEN2) {
      RomGbIcon gi;
      /* BACKLOG #201 F3: rom_gbicon_open()'s two new trailing params are the
       * pass-1/pass-2 progress-reset hook -- this offline generator drives no
       * progress screen, so 0/0 ("no callback"), same as every other silent
       * caller (gb_art_source.c's fetch paths). */
      if (rom_gbicon_open(&gi, rd, NULL, size, scratch, sizeof scratch, 0, 0)) {
        RomGbIconLoc iloc;
        rom_gbicon_save_loc(&gi, &iloc);
        write_payload(out, LOC_KIND_ICON, 2, gi.id_hash, size, &iloc, (uint16_t)sizeof iloc);
        wrote++;
        printf("icon\tgen=2\tid_hash=0x%08X\trec_size=%zu\n", gi.id_hash, sizeof iloc);
      }
    }
  }

  RomGbUi gu;
  if (rom_gbui_open(&gu, rd, NULL, size, scratch, sizeof scratch)) {
    RomGbUiLoc uloc;
    rom_gbui_save_loc(&gu, &uloc);
    write_payload(out, LOC_KIND_UI, gu.gen, gu.id_hash, size, &uloc, (uint16_t)sizeof uloc);
    wrote++;
    printf("ui\tgen=%d\tid_hash=0x%08X\trec_size=%zu\n", gu.gen, gu.id_hash, sizeof uloc);
  }

  fclose(out);
  fclose(g_f);
  printf("RESULT\t%s\t%d\n", wrote ? "OK" : "FAIL", wrote);
  return wrote ? 0 : 1;
}
