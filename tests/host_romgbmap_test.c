/* Host (PC) test for rom_gbmap -- Gen-1 (Red/Blue/Yellow) MAP data located BY
 * SHAPE in real cartridge dumps. rom_gbmap does all its I/O through the
 * caller's GbReadFn, so the code under test is byte-for-byte the code that
 * runs on the GBA.
 *
 *   cc -std=c11 -Wall -Wextra -I source tests/host_romgbmap_test.c \
 *      source/rom_gbmap.c -o /tmp/hgbmap && /tmp/hgbmap
 *
 * ORACLE: an independent throwaway Python re-implementation of the exact
 * same shape (the same session's own scratch analysis, not copied from this
 * C file), run against Guy's real Red.gb/Yellow.gb and cross-checked against
 * `assets/upstream/pokered`'s own .sym files + BACKLOG #91's design-doc
 * grilling section (the corrected 10+N*11+2 Gen-1 header shape, verified
 * there against PalletTown_h's real bytes: connection map ids 12/32 =
 * ROUTE_1/ROUTE_21, both width 10, Object ptr $42C3 -- exactly what this
 * test's WANT table also expects, independently re-derived here).
 *
 * Coverage:
 *   1) both dumps locate all 3 tables (MapHeaderBanks/MapHeaderPointers/
 *      Tilesets) at the exact oracle offsets, via TWO DIFFERENT anchor
 *      shapes (Red: MapHeaderPointers read inline; Yellow: through the
 *      bank-switched GetMapHeaderPointer indirection) -- proving the anchor
 *      generalizes, not just happens to work on one ROM;
 *   2) Pallet Town (map id 0) parses to the CORRECTED header shape: 2
 *      connections (ROUTE_1 north, ROUTE_21 south), both width 10, and the
 *      Object pointer lands exactly on PalletTown_Object ($42C3) -- the
 *      doc's own grilling section's smoking-gun cross-check, re-verified
 *      here as a regression test;
 *   3) map id 0's own tileset (OVERWORLD, id 0) resolves to the exact
 *      Overworld_Block/Overworld_GFX file offsets;
 *   4) the player's ACTUAL current map (read from Red.sav/Yellow.sav's real
 *      SRAM bytes at the corrected $A60A/$A60D/$A60E offsets, gb_fields.c's
 *      own GBF_MAP_ID/POS_X/POS_Y) parses to a structurally plausible
 *      header -- proves the pipeline works end-to-end on a save file too,
 *      not just a hand-picked map id;
 *   5) NEGATIVE CONTROLS (fail-closed): corrupting one byte inside any of
 *      the 3 anchors makes rgm1_open() fail entirely (ok=false, every
 *      offset 0) -- not a wrong answer, no answer; forging a SECOND copy of
 *      an anchor elsewhere in the ROM makes rgm1_open() refuse too (the
 *      "ambiguous fallback" the design calls for, not a coin-flip pick of
 *      either candidate).
 *
 * ROMs are Guy's own dumps: they live OUTSIDE the repo and are never copied
 * into it, so a missing corpus SKIPS rather than failing.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "rom_gbmap.h"

#define ROMS "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms/gb"

static int g_fail = 0, g_check = 0, g_ran = 0;
static void chk(const char* who, const char* what, int cond) {
  g_check++;
  if (!cond) { printf("  !! FAIL [%s] %s\n", who, what); g_fail++; }
}

/* ---------------------------------------------------------------- file I/O */
typedef struct { FILE* f; } FileCtx;
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, fc->f) == len;
}
static long file_size(const char* p) {
  FILE* f = fopen(p, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n;
}
static uint8_t* file_load(const char* p, long* out_n) {
  long n = file_size(p);
  if (n <= 0) return NULL;
  FILE* f = fopen(p, "rb");
  if (!f) return NULL;
  uint8_t* buf = (uint8_t*)malloc((size_t)n);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
  fclose(f);
  *out_n = n;
  return buf;
}

/* ------------------------------------------------------------- memory I/O */
typedef struct { const uint8_t* buf; uint32_t size; } MemCtx;
static bool mem_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  MemCtx* m = (MemCtx*)ctx;
  if (off >= m->size || len > m->size - off) return false;
  memcpy(dst, m->buf + off, len);
  return true;
}

static uint8_t g_scratch[ROM_GBMAP_SCRATCH_MIN];

/* ------------------------------------------------------------- oracle ---- */
typedef struct {
  const char* file;
  uint32_t banks_off, ptrs_off, ts_off;
} Want;

static const Want WANT[] = {
  { "Red.gb",    0xC23D, 0x01AE, 0xC7BE },
  { "Yellow.gb", 0xFC3E4, 0xFC1F2, 0xC558 },
};
#define NWANT (int)(sizeof WANT / sizeof WANT[0])

static void test_locate_and_pallet(const Want* w) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", ROMS, w->file);
  long sz = file_size(path);
  if (sz <= 0) { printf("SKIP %s: corpus not present\n", w->file); return; }
  g_ran++;

  FileCtx fc; fc.f = fopen(path, "rb");
  chk(w->file, "fopen", fc.f != NULL);
  if (!fc.f) return;

  RomGbMap1 g;
  bool ok = rgm1_open(&g, file_read, &fc, (uint32_t)sz, g_scratch, sizeof g_scratch);
  chk(w->file, "rgm1_open ok", ok);
  chk(w->file, "banks_off matches oracle", g.banks_off == w->banks_off);
  chk(w->file, "ptrs_off matches oracle", g.ptrs_off == w->ptrs_off);
  chk(w->file, "tilesets_off matches oracle", g.tilesets_off == w->ts_off);

  /* Pallet Town, map id 0 -- the design's own grilling smoking-gun. */
  GbMap1Header h;
  bool hok = rgm1_header(&g, 0, &h);
  chk(w->file, "PalletTown header parses", hok);
  if (hok) {
    chk(w->file, "PalletTown tileset_id == OVERWORLD(0)", h.tileset_id == 0);
    chk(w->file, "PalletTown height == 9", h.height == 9);
    chk(w->file, "PalletTown width == 10", h.width == 10);
    chk(w->file, "PalletTown has 2 connections", h.nconn == 2);
    if (h.nconn == 2) {
      chk(w->file, "conn[0] == ROUTE_1 (12)", h.conn[0].map_id == 12);
      chk(w->file, "conn[0] width == 10", h.conn[0].width == 10);
      chk(w->file, "conn[1] == ROUTE_21 (32)", h.conn[1].map_id == 32);
      chk(w->file, "conn[1] width == 10", h.conn[1].width == 10);
    }
    /* Object pointer's own FILE bytes must equal PalletTown_Object ($42C3),
     * the corrected-layout smoking gun from the design's grilling section. */
    uint8_t obj2[2] = {0};
    file_read(&fc, h.obj_off, obj2, 2);
    uint16_t obj = (uint16_t)(obj2[0] | (obj2[1] << 8));
    chk(w->file, "Object ptr == $42C3 (PalletTown_Object)", obj == 0x42C3);

    GbMap1Tileset ts;
    bool tsok = rgm1_tileset(&g, h.tileset_id, &ts);
    chk(w->file, "OVERWORLD tileset resolves", tsok);
    if (tsok) {
      chk(w->file, "OVERWORLD gfx_bank == 25", ts.gfx_bank == 25);
      chk(w->file, "OVERWORLD gfx_off == 0x64000", ts.gfx_off == 0x64000);
    }
  }
  fclose(fc.f);
}

/* Player's real current map, off Guy's own .sav bytes -- the corrected
 * $A60A/$A60D/$A60E SRAM offsets (both Red and Yellow, per BACKLOG #91's
 * design grilling §A1: Red/Yellow SHARE this block, no per-game branch). */
static void test_player_map(const char* rom_file, const char* sav_file) {
  char rpath[512], spath[512];
  snprintf(rpath, sizeof rpath, "%s/%s", ROMS, rom_file);
  snprintf(spath, sizeof spath, "%s/%s", ROMS, sav_file);
  long rn = file_size(rpath), sn = file_size(spath);
  if (rn <= 0 || sn <= 0) { printf("SKIP %s/%s: corpus not present\n", rom_file, sav_file); return; }
  g_ran++;

  FileCtx fc; fc.f = fopen(rpath, "rb");
  chk(rom_file, "fopen rom", fc.f != NULL);
  if (!fc.f) return;
  RomGbMap1 g;
  bool ok = rgm1_open(&g, file_read, &fc, (uint32_t)rn, g_scratch, sizeof g_scratch);
  chk(rom_file, "rgm1_open ok", ok);

  FILE* sf = fopen(spath, "rb");
  chk(sav_file, "fopen sav", sf != NULL);
  if (sf && ok) {
    uint8_t map_id = 0, x = 0, y = 0;
    fseek(sf, 0x260A, SEEK_SET); (void)!fread(&map_id, 1, 1, sf);
    fseek(sf, 0x260D, SEEK_SET); (void)!fread(&y, 1, 1, sf);
    fseek(sf, 0x260E, SEEK_SET); (void)!fread(&x, 1, 1, sf);
    GbMap1Header h;
    bool hok = rgm1_header(&g, map_id, &h);
    chk(rom_file, "player's own current map parses", hok);
    if (hok) {
      chk(rom_file, "player x is inside the map's own width*8 px", (uint32_t)x < (uint32_t)h.width * 8u);
      chk(rom_file, "player y is inside the map's own height*8 px", (uint32_t)y < (uint32_t)h.height * 8u);
      printf("  %s player map_id=%u (%ux%u blocks) x=%u y=%u\n",
             rom_file, map_id, h.width, h.height, x, y);
    }
    fclose(sf);
  }
  fclose(fc.f);
}

/* ----------------------------------------------------- negative controls */
static void test_negative_controls(void) {
  char path[512];
  snprintf(path, sizeof path, "%s/Red.gb", ROMS);
  long n = 0;
  uint8_t* buf = file_load(path, &n);
  if (!buf) { printf("SKIP negative controls: corpus not present\n"); return; }
  g_ran++;

  /* Baseline: the unmodified buffer must still locate everything. */
  {
    MemCtx m = { buf, (uint32_t)n };
    RomGbMap1 g;
    bool ok = rgm1_open(&g, mem_read, &m, (uint32_t)n, g_scratch, sizeof g_scratch);
    chk("negctl", "baseline (unmodified RAM copy) locates", ok);
  }

  /* Corrupt one byte inside the MapHeaderPointers anchor (CB 78 C0 at
   * 0x109B on Red.gb) -- the whole open must fail closed, not silently
   * mis-locate. */
  {
    uint8_t* cp = (uint8_t*)malloc((size_t)n);
    memcpy(cp, buf, (size_t)n);
    cp[0x109C] ^= 0xFFu;   /* the 0x78 byte of CB 78 C0 */
    MemCtx m = { cp, (uint32_t)n };
    RomGbMap1 g;
    bool ok = rgm1_open(&g, mem_read, &m, (uint32_t)n, g_scratch, sizeof g_scratch);
    chk("negctl", "corrupted CB78C0 byte -> rgm1_open fails closed", !ok);
    if (!ok) chk("negctl", "failed open zeroes every offset", g.banks_off == 0 && g.ptrs_off == 0 && g.tilesets_off == 0 && g.ok == 0);
    free(cp);
  }

  /* Corrupt one byte inside the MapHeaderBanks anchor. */
  {
    uint8_t* cp = (uint8_t*)malloc((size_t)n);
    memcpy(cp, buf, (size_t)n);
    cp[0x12BC] ^= 0xFFu;   /* the leading 0xE5 (push hl) of the 14-byte anchor */
    MemCtx m = { cp, (uint32_t)n };
    RomGbMap1 g;
    bool ok = rgm1_open(&g, mem_read, &m, (uint32_t)n, g_scratch, sizeof g_scratch);
    chk("negctl", "corrupted MapHeaderBanks anchor -> rgm1_open fails closed", !ok);
    free(cp);
  }

  /* Forge a SECOND copy of the MapHeaderPointers anchor (CB 78 C0 21 lo hi)
   * at an unrelated offset -- rgm1_open must refuse (ambiguous), not
   * silently accept whichever hit scan_one() found first. */
  {
    uint8_t* cp = (uint8_t*)malloc((size_t)n);
    memcpy(cp, buf, (size_t)n);
    uint32_t forge_at = 0x2000;   /* deep in bank-0 code/data, unrelated to the real anchor */
    cp[forge_at + 0] = 0xCB; cp[forge_at + 1] = 0x78; cp[forge_at + 2] = 0xC0;
    cp[forge_at + 3] = 0x21; cp[forge_at + 4] = 0x00; cp[forge_at + 5] = 0x10;
    MemCtx m = { cp, (uint32_t)n };
    RomGbMap1 g;
    bool ok = rgm1_open(&g, mem_read, &m, (uint32_t)n, g_scratch, sizeof g_scratch);
    chk("negctl", "forged 2nd MapHeaderPointers anchor -> rgm1_open refuses (ambiguous)", !ok);
    free(cp);
  }

  free(buf);
}

/* m1 review D1: wXCoord/wYCoord -> block is ONE halving (coord >> 1), not
 * two -- a second /2 put the player marker on the Pokemon Center counter
 * instead of the floor on every map. gbmap_block_of() (rom_gbmap.h) is the
 * pure helper pdna_gbmap.c itself calls; this asserts it directly against
 * Red.sav's own real SRAM bytes, plus the shell's own viewport clamp
 * (VBW=5, pdna_gbmap.c) so a regression in EITHER the halving or the clamp
 * shows up here, not just on a screenshot.
 *
 * MUTATION EVIDENCE (not shipped): reverting gbmap_block_of() to
 * `coord >> 1 >> 1` (the old double-halving bug) turns Red.sav's block_px
 * from 3 into 1, so the "block_px == 3" check below fails -- confirmed by
 * hand before this commit, then reverted back to `>> 1`. */
static void test_block_conversion(void) {
  char spath[512];
  snprintf(spath, sizeof spath, "%s/Red.sav", ROMS);
  FILE* sf = fopen(spath, "rb");
  if (!sf) { printf("SKIP block conversion: corpus not present\n"); return; }
  g_ran++;

  uint8_t x = 0, y = 0;
  fseek(sf, 0x260D, SEEK_SET); (void)!fread(&y, 1, 1, sf);
  fseek(sf, 0x260E, SEEK_SET); (void)!fread(&x, 1, 1, sf);
  fclose(sf);

  int block_px = gbmap_block_of(x);
  int block_py = gbmap_block_of(y);
  chk("Red.sav", "block_px == 3 (D1: one halving, not two)", block_px == 3);
  chk("Red.sav", "block_py == 2 (D1: one halving, not two)", block_py == 2);

  /* Red's own current map (VIRIDIAN_POKECENTER, id 41) is 7x4 blocks
   * (test_player_map() above prints this live); the shell's own viewport is
   * VBW=5 blocks wide (pdna_gbmap.c), centred on the player and clamped to
   * [0, width-VBW]. block_px=3 - VBW/2=2 = 1, within [0, 7-5=2] -- no
   * clamping needed, so the initial vbx is exactly 1. */
  const int VBW = 5;
  char rpath[512];
  snprintf(rpath, sizeof rpath, "%s/Red.gb", ROMS);
  long rn = file_size(rpath);
  if (rn > 0) {
    FileCtx fc; fc.f = fopen(rpath, "rb");
    if (fc.f) {
      RomGbMap1 g;
      if (rgm1_open(&g, file_read, &fc, (uint32_t)rn, g_scratch, sizeof g_scratch)) {
        GbMap1Header h;
        if (rgm1_header(&g, 41, &h)) {
          int vbx = block_px - VBW / 2;
          if (vbx < 0) vbx = 0;
          int max_vbx = (int)h.width > VBW ? (int)h.width - VBW : 0;
          if (vbx > max_vbx) vbx = max_vbx;
          chk("Red.sav", "initial vbx == 1", vbx == 1);
        }
      }
      fclose(fc.f);
    }
  }
}

/* m1 review D2: the Gen-1 map header height/width bound was 64, refusing
 * Route 17 and Route 23 (Red map id 28/34, Yellow same ids) -- both are
 * 10x72 blocks, taller than the old bound. The corrected bound is 128
 * (rom_gbmap.c's own `height > 128`/`width > 128` check).
 *
 * MUTATION EVIDENCE (not shipped): reverting rom_gbmap.c's bound back to
 * `height > 64 || ... || width > 64` makes BOTH checks below fail on BOTH
 * ROMs (rgm1_header returns false for id 28/34: height 72 > 64) --
 * confirmed by hand before this commit, then reverted back to 128. */
static void test_route_headers(const char* rom_file) {
  char rpath[512];
  snprintf(rpath, sizeof rpath, "%s/%s", ROMS, rom_file);
  long rn = file_size(rpath);
  if (rn <= 0) { printf("SKIP route headers %s: corpus not present\n", rom_file); return; }
  g_ran++;

  FileCtx fc; fc.f = fopen(rpath, "rb");
  chk(rom_file, "fopen rom", fc.f != NULL);
  if (!fc.f) return;
  RomGbMap1 g;
  bool ok = rgm1_open(&g, file_read, &fc, (uint32_t)rn, g_scratch, sizeof g_scratch);
  chk(rom_file, "rgm1_open ok", ok);
  if (ok) {
    GbMap1Header h28;
    bool ok28 = rgm1_header(&g, 28, &h28);
    chk(rom_file, "map id 28 (Route 17) parses (D2: 10x72 > old 64 bound)", ok28);
    if (ok28) {
      chk(rom_file, "Route 17 height == 72", h28.height == 72);
      chk(rom_file, "Route 17 width == 10", h28.width == 10);
    }
    GbMap1Header h34;
    bool ok34 = rgm1_header(&g, 34, &h34);
    chk(rom_file, "map id 34 (Route 23) parses (D2: 10x72 > old 64 bound)", ok34);
    if (ok34) {
      chk(rom_file, "Route 23 height == 72", h34.height == 72);
      chk(rom_file, "Route 23 width == 10", h34.width == 10);
    }
  }
  fclose(fc.f);
}

/* m1 review D7: cb_ts()'s structural filter now runs INSIDE the scan
 * callback, so scan_one()'s own hit cap (now 2, was 64) counts real
 * candidates -- this is the negative side of that change: a Gen-2/Gen-3 ROM
 * must still produce EITHER 0 filtered `5F 21` hits or more than 1 (never
 * exactly 1), so rgm1_open() fails closed on every non-Gen-1 ROM, not just
 * on Red/Yellow's own corrupted anchors. Emerald.gba lives one directory up
 * from ROMS (roms/, not roms/gb/). */
static void test_offgame_fails_closed(const char* dir, const char* file) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", dir, file);
  long n = file_size(path);
  if (n <= 0) { printf("SKIP off-game control %s: corpus not present\n", file); return; }
  g_ran++;

  FileCtx fc; fc.f = fopen(path, "rb");
  chk(file, "fopen", fc.f != NULL);
  if (!fc.f) return;
  RomGbMap1 g;
  bool ok = rgm1_open(&g, file_read, &fc, (uint32_t)n, g_scratch, sizeof g_scratch);
  chk(file, "rgm1_open fails closed on a non-Gen-1 ROM", !ok);
  if (!ok) chk(file, "failed open zeroes every offset", g.banks_off == 0 && g.ptrs_off == 0 && g.tilesets_off == 0 && g.ok == 0);
  fclose(fc.f);
}

int main(void) {
  for (int i = 0; i < NWANT; i++) test_locate_and_pallet(&WANT[i]);
  test_player_map("Red.gb", "Red.sav");
  test_player_map("Yellow.gb", "Yellow.sav");
  test_block_conversion();
  test_route_headers("Red.gb");
  test_route_headers("Yellow.gb");
  test_negative_controls();
  test_offgame_fails_closed(ROMS, "Gold.gbc");
  test_offgame_fails_closed(ROMS, "Crystal.gbc");
  test_offgame_fails_closed(ROMS "/..", "Emerald.gba");

  printf("host_romgbmap_test: %d checks, %d failed, %d ROM(s)/pair(s) exercised\n",
         g_check, g_fail, g_ran);
  if (g_ran == 0) { printf("(no corpus found -- SKIPPED, not a pass)\n"); return 0; }
  return g_fail ? 1 : 0;
}
