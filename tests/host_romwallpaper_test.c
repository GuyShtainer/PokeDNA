/* Host (PC) test for rom_wallpaper — box wallpapers read from REAL retail ROMs.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -I source tests/host_romwallpaper_test.c source/rom_wallpaper.c \
 *      source/map_render.c source/rom_map.c -o /tmp/hrwp && /tmp/hrwp
 *
 * All I/O goes through the RomCtx callback, so the code under test is byte-for-byte
 * the code pdna_box.c's ROM rung runs on the GBA (fused today, SD-registered later).
 *
 * What it proves:
 *   1) rom_wallpaper_open pins Emerald (BPEE r0), FireRed (BPRE r1) and LeafGreen
 *      (BPGE r0), and FAILS CLOSED on Ruby/Sapphire (different row struct, not
 *      parsed by this module — see rom_wallpaper.h SCOPE) and on any revision not
 *      in k_pins (FireRed r0, LeafGreen r1 — never measured, deliberately absent);
 *   2) all 16 standard wallpapers decompress: the tilemap is EXACTLY 720 B every
 *      time, the tile blob is a whole multiple of 32 B and never exceeds
 *      ROM_WP_TILES_MAX_BYTES (the whole reason this rung needs no new EWRAM);
 *   3) every one of a wallpaper's 360 map entries names a tid inside its own tiles
 *      blob (DESIGN.md's "every row is self-contained" claim, re-checked here) and
 *      rom_wallpaper_expand_tile succeeds for every one of them, never touching a
 *      byte outside the tiles buffer;
 *   4) determinism: decompressing the SAME wallpaper twice yields byte-identical
 *      map and tile bytes — the property pdna_box.c's ROM-rung verify (re-decompress
 *      and compare, matching wp_copy_verified's retry/abandon shape) depends on;
 *   5) out-of-range wp/tid and a too-small tiles buffer are refused, not truncated.
 *
 * ROMs are the user's own dumps, never part of the repo; missing ROMs SKIP.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "rom_map.h"
#include "rom_wallpaper.h"

static int fails = 0, checks = 0;
static void chk(const char* rom, const char* what, int cond) {
  checks++;
  if (!cond) { printf("FAIL [%s] %s\n", rom, what); fails++; }
}

typedef struct { FILE* f; long calls; } FileCtx;
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  fc->calls++;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, fc->f) == len;
}

static void run_rom(const char* path, const char* name, int expect_open) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP %s (no %s)\n", name, path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc = { f, 0 };
  RomCtx rc;
  if (!rom_open(&rc, file_read, &fc, (uint32_t)sz)) {
    chk(name, "rom_open accepts the retail dump", 0);
    fclose(f); return;
  }

  RomWallpaper rw;
  int ok = rom_wallpaper_open(&rw, &rc);
  chk(name, expect_open ? "wallpaper table pins + self-checks" : "fails CLOSED (unpinned)",
      ok == expect_open);
  if (!ok) { fclose(f); return; }

  static uint8_t tiles_a[ROM_WP_TILES_MAX_BYTES], tiles_b[ROM_WP_TILES_MAX_BYTES];
  static uint16_t map_a[ROM_WP_MAP_ENTRIES], map_b[ROM_WP_MAP_ENTRIES];

  for (int wp = 0; wp < ROM_WP_COUNT; wp++) {
    char tag[48]; sprintf(tag, "wp %d", wp);

    chk(name, tag, rom_wallpaper_map(&rw, wp, map_a));
    uint32_t tbytes = 0;
    int tok = rom_wallpaper_tiles(&rw, wp, tiles_a, sizeof tiles_a, &tbytes);
    chk(name, tag, tok && tbytes > 0 && (tbytes % 32u) == 0 &&
                   tbytes <= ROM_WP_TILES_MAX_BYTES);

    uint16_t pal[ROM_WP_PAL_BANKS][16];
    chk(name, tag, rom_wallpaper_pal(&rw, wp, pal));

    /* every map entry's tid is inside this wallpaper's own tiles blob, and every
     * one expands without ever reading past tiles_a[0..tbytes) */
    int bad_tid = 0, bad_expand = 0;
    for (int c = 0; c < ROM_WP_MAP_ENTRIES; c++) {
      uint16_t e = map_a[c];
      uint16_t tid = (uint16_t)(e & 0x3FFu);
      int hf = (e >> 10) & 1, vf = (e >> 11) & 1, bank = (e >> 12) & 0xF;
      if ((uint32_t)tid * 32u + 32u > tbytes) { bad_tid++; continue; }
      uint16_t out[64];
      if (!rom_wallpaper_expand_tile(tiles_a, tbytes, tid, hf, vf, pal[bank % ROM_WP_PAL_BANKS],
                                     out))
        bad_expand++;
    }
    chk(name, tag, bad_tid == 0);
    chk(name, tag, bad_expand == 0);

    /* determinism: a second independent decompress of the SAME wallpaper agrees
     * byte-for-byte -- the property the caller-side verify/retry depends on */
    uint16_t map2[ROM_WP_MAP_ENTRIES];
    chk(name, tag, rom_wallpaper_map(&rw, wp, map2) &&
                   memcmp(map_a, map2, sizeof map_a) == 0);
    uint32_t t2bytes = 0;
    chk(name, tag, rom_wallpaper_tiles(&rw, wp, tiles_b, sizeof tiles_b, &t2bytes) &&
                   t2bytes == tbytes && memcmp(tiles_a, tiles_b, tbytes) == 0);
  }
  (void)map_b;

  /* out-of-range everything is refused, not truncated */
  chk(name, "wp -1 refused", !rom_wallpaper_map(&rw, -1, map_a));
  chk(name, "wp 16 refused", !rom_wallpaper_map(&rw, ROM_WP_COUNT, map_a));
  uint32_t nb = 0;
  chk(name, "small tiles buffer refused, not truncated",
      !rom_wallpaper_tiles(&rw, 0, tiles_a, 4, &nb) && nb == 0);
  {
    uint16_t out[64], pal16[16] = {0};
    chk(name, "tid past the blob refused", !rom_wallpaper_expand_tile(tiles_a, 32, 5, 0, 0,
                                                                      pal16, out));
  }

  fclose(f);
}

int main(void) {
  const char* dir = getenv("ROMS");
  char p[512];
#define RP(g) (sprintf(p, "%s/%s.gba", dir ? dir : "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms", g), p)
  run_rom(RP("Emerald"),   "Emerald",   1);
  run_rom(RP("FireRed"),   "FireRed",   1);
  run_rom(RP("LeafGreen"), "LeafGreen", 1);
  run_rom(RP("Ruby"),      "Ruby",      0);
  run_rom(RP("Sapphire"),  "Sapphire",  0);
#undef RP

  printf("%d checks, %d fails\n", checks, fails);
  return fails ? 1 : 0;
}
