/* Host (PC) test for gb_lz -- the Gen-2 GBC "LZ" tile-graphics decoder.
 * gb_lz does all its I/O through the caller's GbReadFn, so the code under
 * test is byte-for-byte the code that runs on the GBA. Reads the ROM at
 * TEST TIME only -- ships no data (same posture as host_romgbmap_test.c).
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_gblz_test.c \
 *      source/gb_lz.c source/rom_gbmap2.c source/rom_gbmap.c \
 *      -o /tmp/hgblz && /tmp/hgblz
 *
 * Coverage (docs/briefs/map-g2-brief.md Step 2):
 *   1) walks the located Tilesets table in BOTH Gold.gbc and Crystal.gbc
 *      and decodes every row's GFX blob (29 + 37 = 66 total);
 *   2) asserts, for all 66: decode succeeds, output length is a multiple
 *      of 16, and the measured envelope matches the design doc exactly --
 *      Gold all exactly 1,536 B (96 tiles), Crystal 1,536..3,072 B
 *      (96..192 tiles);
 *   3) mutation/negative controls: truncate a real blob mid-stream (0, no
 *      crash); forge a runaway LONG-form length against a tiny out_cap (0,
 *      output never exceeds out_cap); a back-reference pointing before the
 *      output start (0).
 *
 * ROMs are Guy's own dumps: they live OUTSIDE the repo and are never
 * copied into it, so a missing corpus SKIPS rather than failing.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "gb_lz.h"
#include "rom_gbmap2.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
static void chk(const char* who, const char* what, int cond) {
  g_check++;
  if (!cond) { printf("  !! FAIL [%s] %s\n", who, what); g_fail++; }
}

typedef struct { FILE* f; } FileCtx;
static bool rd_file(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* c = (FileCtx*)ctx;
  if (fseek(c->f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, c->f) == len;
}

typedef struct { const uint8_t* buf; uint32_t len; } MemCtx;
static bool rd_mem(void* ctx, uint32_t off, void* dst, uint32_t len) {
  MemCtx* c = (MemCtx*)ctx;
  if (off >= c->len || len > c->len - off) return false;
  memcpy(dst, c->buf + off, len);
  return true;
}

static uint8_t g_out[8192];

static void test_rom(const char* rom_file, const char* name,
                      uint32_t want_min, uint32_t want_max, int uniform) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", ROMS, rom_file);
  FILE* f = fopen(path, "rb");
  if (!f) { printf("  [%s] SKIP (corpus absent)\n", name); return; }
  g_ran = 1;
  fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
  FileCtx ctx = { f };

  static uint8_t scratch[4096];
  RomGbMap2 g;
  bool ok = rgm2_open(&g, rd_file, &ctx, (uint32_t)sz, scratch, sizeof scratch);
  chk(name, "locator opens", ok);
  if (!ok) { fclose(f); return; }

  int decoded = 0;
  uint32_t minlen = 0xFFFFFFFFu, maxlen = 0;
  for (uint32_t i = 0; i < g.n_tilesets; i++) {
    GbMap2Tileset ts;
    char who[64]; snprintf(who, sizeof who, "%s ts%u", name, i);
    chk(who, "tileset row resolves", rgm2_tileset(&g, (uint8_t)i, &ts));
    if (!rgm2_tileset(&g, (uint8_t)i, &ts)) continue;

    uint32_t n = gb_lz_decode(rd_file, &ctx, ts.gfx_off, (uint32_t)sz - ts.gfx_off,
                               g_out, sizeof g_out);
    chk(who, "GFX blob decodes", n > 0);
    if (n == 0) continue;
    chk(who, "decoded length is a multiple of 16", (n % 16u) == 0u);
    chk(who, "decoded length within [96,4096] tiles*16", n >= 96u * 16u && n <= 4096u);
    if (n < minlen) minlen = n;
    if (n > maxlen) maxlen = n;
    decoded++;
  }
  printf("  [%s] tilesets=%u decoded=%d min=%u max=%u\n", name, g.n_tilesets, decoded, minlen, maxlen);
  chk(name, "all rows decoded", decoded == (int)g.n_tilesets);
  chk(name, "min decoded length matches design doc", minlen == want_min);
  chk(name, "max decoded length matches design doc", maxlen == want_max);
  if (uniform) chk(name, "every blob is exactly the uniform size", minlen == maxlen);

  fclose(f);
}

static void test_mutations(void) {
  uint8_t out[8192];

  /* truncated: LITERAL length 5 claimed, only 2 bytes present */
  { uint8_t s[] = { 0x04, 0xAA, 0xBB };
    MemCtx c = { s, sizeof s };
    uint32_t n = gb_lz_decode(rd_mem, &c, 0, sizeof s, out, sizeof out);
    chk("mutation", "truncated LITERAL -> 0", n == 0);
  }

  /* runaway LONG-form length (1024) against a tiny out_cap */
  { uint8_t s[3 + 1024 + 1];
    s[0] = 0xE3; s[1] = 0xFF;   /* cmd7 -> real cmd0 LITERAL, length=1024 */
    for (int i = 0; i < 1024; i++) s[2 + i] = 0x11;
    s[2 + 1024] = 0xFF;
    MemCtx c = { s, sizeof s };
    uint32_t n = gb_lz_decode(rd_mem, &c, 0, sizeof s, out, 10);
    chk("mutation", "runaway LONG vs tiny out_cap -> 0", n == 0);
  }

  /* back-reference before output start (out_len==0, negative dist 1) */
  { uint8_t s[] = { 0x80, 0x80 };
    MemCtx c = { s, sizeof s };
    uint32_t n = gb_lz_decode(rd_mem, &c, 0, sizeof s, out, sizeof out);
    chk("mutation", "backref before output start -> 0", n == 0);
  }

  /* REVERSE valid at command start, runs off the front mid-command */
  { uint8_t s[] = {
      0x02, 0xAA, 0xBB, 0xCC,   /* cmd0 LITERAL len3 */
      0xC4,                     /* cmd6 REVERSE len5 */
      0x00, 0x02,                /* positive offset 2 (absolute) */
      0xFF };
    MemCtx c = { s, sizeof s };
    uint32_t n = gb_lz_decode(rd_mem, &c, 0, sizeof s, out, sizeof out);
    chk("mutation", "REVERSE off-front mid-command -> 0", n == 0);
  }

  /* missing LZ_END entirely within the bank cap */
  { uint8_t s[64]; memset(s, 0x60, sizeof s);  /* cmd3 ZERO len1, repeated */
    MemCtx c = { s, sizeof s };
    uint32_t n = gb_lz_decode(rd_mem, &c, 0, sizeof s, out, sizeof out);
    chk("mutation", "missing LZ_END -> 0", n == 0);
  }

  /* nested LONG (real cmd id == 7) behaves as REPEAT */
  { uint8_t s[] = { 0x60, 0xFC, 0x00, 0x80, 0xFF };  /* ZERO len1, then nested-long len1 REPEAT dist1 */
    MemCtx c = { s, sizeof s };
    uint32_t n = gb_lz_decode(rd_mem, &c, 0, sizeof s, out, sizeof out);
    chk("mutation", "nested LONG -> REPEAT, decodes", n == 2 && out[0] == 0 && out[1] == 0);
  }
}

int main(void) {
  test_rom("Gold.gbc", "Gold", 1536, 1536, 1);
  test_rom("Crystal.gbc", "Crystal", 1536, 3072, 0);
  test_mutations();

  if (!g_ran) { printf("host_gblz_test: SKIP (no corpus)\n"); return 0; }
  printf("host_gblz_test: %d checks, %d failed\n", g_check, g_fail);
  return g_fail ? 1 : 0;
}
