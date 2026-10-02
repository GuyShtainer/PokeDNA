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
 *      [#311: Ruby/Sapphire are now served for AXVE r2 / AXPE r1 ONLY; r0 and a layout lie refuse]
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
 *   5) out-of-range wp/tid and a too-small tiles buffer are refused, not truncated;
 *   6) BACKLOG #294: rom_wallpaper_expand_cell composes the tiled backdrop under the tilemap
 *      (index 0 transparent). On Emerald all 16 standard wallpapers, composed whole
 *      (20x18 cells -> 160x144 RGB15), hash EXACTLY to the compiled wallpapers.c composite
 *      (tools/gen_wallpaper.py's assemble(), FNV-1a over the LE u16 pixels -- goldens
 *      below); FireRed/LeafGreen (no sheet table) compose with a flat interior tone and
 *      never leave a band-cell pixel at the literal white unless the tone itself is white.
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

typedef struct { FILE* f; long calls; long poff; uint8_t pval; } FileCtx;   /* poff < 0: no patch */
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  fc->calls++;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, fc->f) != len) return false;
  if (fc->poff >= 0 && (uint32_t)fc->poff >= off && (uint32_t)fc->poff < off + len)
    ((uint8_t*)dst)[(uint32_t)fc->poff - off] = fc->pval;     /* test-only byte mutation */
  return true;
}

/* FNV-1a 32 over the composed 160x144 image's little-endian u16 pixels (bit 15 cleared). */
/* #311: R/S goldens from an INDEPENDENT Python decoder (own LZ10, backdrop = bank 0 entry 0,
 * raw bank n -> pal bank n); AXVE rev 2 and AXPE rev 1 carry identical wallpaper data. */
static const uint32_t k_rs_golden[ROM_WP_COUNT] = {
  0x9D06EDD1u, 0xBB4576C2u, 0xADB72E42u, 0x692D7B71u, 0x30044116u, 0xD18A722Au, 0xA1CF73BAu, 0x200731C8u,
  0x5DE6E423u, 0x17DB85AAu, 0x3C464B7Cu, 0x5BF22CCEu, 0xE57EEDECu, 0x12241E83u, 0xDC1C5914u, 0x386189C1u,
};
static const uint32_t k_em_golden[ROM_WP_COUNT] = {
  0xFF8FA8AEu, 0xD6199C60u, 0x190381CBu, 0x2C8EEFADu, 0xE6539EF7u, 0x5AF9A66Du, 0x70F2EA42u, 0xAAF78964u,
  0xA1E53319u, 0x899EC365u, 0x856CCDC6u, 0xCB92A806u, 0xC561316Du, 0x3A6F7ED0u, 0x922705A5u, 0x0EE18723u,
};

static uint32_t compose_hash(const RomWallpaper* rw, int wp, const uint8_t* tiles, uint32_t tbytes,
                             const uint16_t* map, const uint16_t pal[ROM_WP_PAL_BANKS][16],
                             int* white_band_px) {
  RomWpBase bs;
  uint32_t h = 0x811C9DC5u;
  int white = 0;
  if (!rom_wallpaper_base(rw, wp, tbytes, &bs)) return 0;
  for (int ty = 0; ty < 18; ty++)
    for (int r = 0; r < 8; r++)
      for (int tx = 0; tx < 20; tx++) {
        uint16_t out[64];
        if (!rom_wallpaper_expand_cell(tiles, tbytes, map[ty * 20 + tx], tx, ty, &bs, pal, out))
          return 0;
        for (int c = 0; c < 8; c++) {
          uint16_t v = (uint16_t)(out[r * 8 + c] & 0x7FFFu);
          h ^= (uint8_t)(v & 0xFFu); h *= 0x01000193u;
          h ^= (uint8_t)(v >> 8);    h *= 0x01000193u;
          if (ty < 3 && (tx < 2 || tx > 17) && v == 0x7FFFu) white++;   /* the 12 blank corner cells */
        }
      }
  if (white_band_px) *white_band_px = white;
  return h;
}

/* Open with one ROM byte mutated (poff/pval) and check the verdict. Proves the pin + the
 * R/S open-time verify refuse a wrong revision / a wrong layout. */
static void open_patched(const char* path, const char* name, long poff, uint8_t pval, int expect_open) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP %s (no %s)\n", name, path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc = { f, 0, poff, pval };
  RomCtx rc;
  RomWallpaper rw;
  int opened = rom_open(&rc, file_read, &fc, (uint32_t)sz) && rom_wallpaper_open(&rw, &rc);
  chk(name, expect_open ? "opens" : "refused (fail closed)", opened == expect_open);
  fclose(f);
}

static void run_rom(const char* path, const char* name, int expect_open) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP %s (no %s)\n", name, path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc = { f, 0, -1, 0 };
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
      if (rw.rs && bank >= ROM_WP_PAL_BANKS) { bad_tid++; continue; }   /* R/S bank field in {0,1,2} */
      if ((uint32_t)tid * 32u + 32u > tbytes) { bad_tid++; continue; }
      uint16_t out[64];
      if (!rom_wallpaper_expand_tile(tiles_a, tbytes, tid, hf, vf,
                                     pal[rw.rs ? bank : rom_wallpaper_pal_bank(bank)], out))
        bad_expand++;
    }
    chk(name, tag, bad_tid == 0);
    chk(name, tag, bad_expand == 0);

    /* BACKLOG #294: the composed wallpaper (backdrop + overlay) */
    {
      int white = 0;
      uint32_t h = compose_hash(&rw, wp, tiles_a, tbytes, map_a, pal, &white);
      chk(name, "composes (no refusal)", h != 0);
      if (rc.kind == ROM_EMERALD) {
        char t2[64]; sprintf(t2, "wp %d composite == compiled wallpapers.c (golden hash)", wp);
        chk(name, t2, h == k_em_golden[wp]);
      } else if (rw.rs) {
        char t2[64]; sprintf(t2, "wp %d R/S composite == independent oracle (golden hash)", wp);
        chk(name, t2, h == k_rs_golden[wp]);
        /* 3 banks parsed: the R/S palette blob is 96 B, bank 2 is the field/interior bank */
        int any2 = 0; for (int c = 1; c < 16; c++) if (pal[2][c]) any2 = 1;
        chk(name, "R/S pal bank 2 parsed (non-empty)", any2);
        chk(name, "R/S has no backdrop sheet (the map covers the box)",
            rom_wallpaper_base(&rw, wp, tbytes, &(RomWpBase){0}) == 1);
      } else {
        uint16_t tone = pal[ROM_WP_EM_BANKS - 1][1];
        chk(name, "FR/LG flat tone: corner cells not literal white unless the tone is", tone == 0x7FFF || tone == 0 || white == 0);
      }
    }

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

  {
    RomWpBase bz;
    chk(name, "base: NULL out refused", !rom_wallpaper_base(&rw, 0, 4096, NULL));
    chk(name, "base: wp -1 refused, out zeroed", !rom_wallpaper_base(&rw, -1, 4096, &bz) && bz.cols == 0);
    if (rc.kind == ROM_EMERALD) {
      RomWpBase b0;
      chk(name, "cell_key: base built", rom_wallpaper_base(&rw, 0, 63u * 32u, &b0) && b0.cols == 4 && b0.rows == 2);
      chk(name, "cell_key: phase changes the key (skip cache must not reuse a tile across phases)",
          rom_wallpaper_cell_key(&b0, 0x2037, 0, 0) != rom_wallpaper_cell_key(&b0, 0x2037, 1, 0) &&
          rom_wallpaper_cell_key(&b0, 0x2037, 0, 0) != rom_wallpaper_cell_key(&b0, 0x2037, 0, 1));
      chk(name, "cell_key: same phase, same key; never negative",
          rom_wallpaper_cell_key(&b0, 0x2037, 0, 0) == rom_wallpaper_cell_key(&b0, 0x2037, 4, 2) &&
          rom_wallpaper_cell_key(&b0, 0xFFFF, 3, 1) >= 0);
    }
    if (rc.kind == ROM_EMERALD)
      chk(name, "base: a blob shorter than the sheet refused", !rom_wallpaper_base(&rw, 0, 32u * 7u, &bz));
  }

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
  run_rom(RP("Ruby"),      "Ruby",      1);
  run_rom(RP("Sapphire"),  "Sapphire",  1);
  open_patched(RP("Ruby"), "Ruby r0 (version byte patched)", 0xBC, 0, 0);
  open_patched(RP("Sapphire"), "Sapphire r0 (version byte patched)", 0xBC, 0, 0);
  open_patched(RP("Ruby"), "Ruby, row 9 tilemap ptr off (layout lie)", 0x3BB104 + 9 * 16 + 8, 0xFF, 0);
  open_patched(RP("Sapphire"), "Sapphire, row 15 tilemap ptr off (layout lie)", 0x3BB160 + 15 * 16 + 8, 0xFF, 0);
  open_patched(RP("Ruby"), "Ruby, unpatched control (same path opens)", 0x7FFFFF, 0, 1);
#undef RP

  printf("%d checks, %d fails\n", checks, fails);
  return fails ? 1 : 0;
}
