/* gbui_dump_driver.c -- the C half of tools/gbui_dump.py. Compiled on the fly
 * against the SHIPPED source/rom_gbui.c (+ gb_sprite_codec.c for the Gen-1
 * player pic's decode), never a re-implementation: this proves the dump
 * matches what the GBA build will actually locate, not a second opinion.
 *
 * Usage: gbui_dump_driver <rom-path> [output-dir]
 * Prints one line per located graphic: `name<TAB>offset_hex<TAB>note`, then
 * `RESULT<TAB>ok|FAIL`. With an output dir, also writes one .ppm per located
 * graphic (P6, uncompressed -- Preview/GIMP/ImageMagick all open it directly;
 * chosen over PNG so this stays a single self-contained C file with no zlib/
 * CRC dependency for what is a throwaway dev tool, never shipped ROM code).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "rom_gbui.h"
#include "gb_sprite_codec.h"

static FILE* g_f;
static bool rd(void* ctx, uint32_t off, void* dst, uint32_t len) {
  (void)ctx;
  if (fseek(g_f, (long)off, SEEK_SET)) return false;
  return fread(dst, 1, len, g_f) == len;
}
static uint32_t fsize(FILE* f) {
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); return (uint32_t)n;
}

static void rgb15_to_rgb8(uint16_t c, uint8_t out[3]) {
  uint8_t r = (uint8_t)(c & 0x1F), gr = (uint8_t)((c >> 5) & 0x1F), b = (uint8_t)((c >> 10) & 0x1F);
  out[0] = (uint8_t)((r << 3) | (r >> 2));
  out[1] = (uint8_t)((gr << 3) | (gr >> 2));
  out[2] = (uint8_t)((b << 3) | (b >> 2));
}

/* Write a cols-wide grid of 8x8 tiles as an uncompressed PPM. `px` is
 * ntiles*64 RGB15 values, tile-major, row-major within each tile. */
static void write_grid_ppm(const char* path, const uint16_t* px, uint32_t ntiles, uint32_t cols) {
  uint32_t rows = (ntiles + cols - 1) / cols;
  uint32_t w = cols * 8, h = rows * 8;
  FILE* o = fopen(path, "wb");
  if (!o) return;
  fprintf(o, "P6\n%u %u\n255\n", w, h);
  uint8_t* row = (uint8_t*)malloc((size_t)w * 3);
  for (uint32_t y = 0; y < h; y++) {
    uint32_t ty = y / 8, iy = y % 8;
    for (uint32_t x = 0; x < w; x++) {
      uint32_t tx = x / 8, ix = x % 8;
      uint32_t t = ty * cols + tx;
      uint8_t rgb[3] = {0, 0, 0};
      if (t < ntiles) rgb15_to_rgb8(px[t * 64 + iy * 8 + ix], rgb);
      row[x * 3 + 0] = rgb[0]; row[x * 3 + 1] = rgb[1]; row[x * 3 + 2] = rgb[2];
    }
    fwrite(row, 1, (size_t)w * 3, o);
  }
  free(row);
  fclose(o);
}

static void dump_block(RomGbUi* gu, const char* outdir, const char* name,
                       uint32_t off, uint8_t bpp, uint32_t ntiles, uint32_t cols) {
  if (!off || !outdir) return;
  uint16_t* px = (uint16_t*)malloc((size_t)ntiles * 64 * sizeof(uint16_t));
  if (!px) return;
  for (uint32_t i = 0; i < ntiles; i++) rom_gbui_tile(gu, off, i, bpp, 0, px + i * 64);
  char path[600];
  snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
  write_grid_ppm(path, px, ntiles, cols);
  free(px);
}

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <rom> [outdir]\n", argv[0]); return 2; }
  const char* outdir = (argc >= 3) ? argv[2] : NULL;
  if (outdir) mkdir(outdir, 0755);

  g_f = fopen(argv[1], "rb");
  if (!g_f) { printf("RESULT\tFAIL\topen\n"); return 1; }
  uint32_t sz = fsize(g_f);
  static uint8_t scratch[ROM_GBUI_SCRATCH_MIN];
  RomGbUi gu;
  int ok = rom_gbui_open(&gu, rd, NULL, sz, scratch, sizeof scratch);

  printf("gen\t%d\n", gu.gen);
  printf("banks\t%d\n", gu.banks);
  printf("font\t0x%X\n", gu.font);
  printf("textbox\t0x%X\n", gu.textbox);
  printf("cardframe\t0x%X\n", gu.cardframe);
  printf("badges\t0x%X\n", gu.badges);
  printf("leaders\t0x%X\n", gu.leaders);
  printf("playerpic\t0x%X\n", gu.playerpic);
  printf("playerpic_bank\t0x%X\n", gu.playerpic_bank);
  printf("frames\t0x%X\n", gu.frames);
  printf("fontextra\t0x%X\n", gu.fontextra);
  printf("cardpic_m\t0x%X\n", gu.cardpic_m);
  printf("cardpic_f\t0x%X\n", gu.cardpic_f);
  printf("cardgfx\t0x%X\n", gu.cardgfx);
  printf("pack_m\t0x%X\n", gu.pack_m);
  printf("pack_f\t0x%X\n", gu.pack_f);
  printf("RESULT\t%s\n", ok ? "OK" : "FAIL");

  if (outdir) {
    dump_block(&gu, outdir, "font", gu.font, 1, 128, 16);
    dump_block(&gu, outdir, "textbox", gu.textbox, 2, 32, 16);
    dump_block(&gu, outdir, "cardframe", gu.cardframe, 2, 40, 8);
    dump_block(&gu, outdir, "badges", gu.badges, 2, 64, 8);
    dump_block(&gu, outdir, "leaders", gu.leaders, 2, 86, 10);
    dump_block(&gu, outdir, "frames", gu.frames, 1, 54, 6);
    dump_block(&gu, outdir, "fontextra", gu.fontextra, 2, 32, 16);
    dump_block(&gu, outdir, "cardpic_m", gu.cardpic_m, 2, 35, 5);
    dump_block(&gu, outdir, "cardpic_f", gu.cardpic_f, 2, 35, 5);
    dump_block(&gu, outdir, "cardgfx", gu.cardgfx, 2, 6, 6);
    dump_block(&gu, outdir, "pack_m", gu.pack_m, 2, 60, 5);
    dump_block(&gu, outdir, "pack_f", gu.pack_f, 2, 60, 5);

    if (gu.playerpic) {
      static GbSprite spr;
      if (gb_sprite_gen1(&spr, rd, NULL, gu.playerpic) == GB_SPRITE_OK) {
        static const uint8_t shade[4][3] = {{248,248,248},{168,168,168},{88,88,88},{16,16,16}};
        char path[600];
        snprintf(path, sizeof path, "%s/playerpic.ppm", outdir);
        FILE* o = fopen(path, "wb");
        if (o) {
          fprintf(o, "P6\n%u %u\n255\n", spr.w, spr.h);
          for (uint32_t i = 0; i < (uint32_t)spr.w * spr.h; i++)
            fwrite(shade[spr.px[i] & 3], 1, 3, o);
          fclose(o);
        }
      }
    }
  }

  fclose(g_f);
  return ok ? 0 : 1;
}
