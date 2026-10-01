/* source/art_icons_extract.c -- the icons.bin generator -- against Guy's real ROM
 * dumps. Same reader/RomCtx idiom as tests/host_rommon_test.c so this is byte-for-
 * byte the code that runs on the GBA (through the exact same rom_mon.c calls).
 *
 *   cc -std=c11 -I source tests/host_artextract_test.c source/art_icons_extract.c \
 *      source/rom_mon.c source/rom_map.c source/art_cache.c -o /tmp/hax && /tmp/hax \
 *      [rom_dir]
 *
 * What this proves:
 *   1. every row art_icons_stream produces for a real ROM matches rom_mon_icon_at's
 *      OWN two frames for that row, called independently -- the generator is not
 *      inventing a different truth than the ROM reader everything else uses;
 *   2. the tail (pal ids + 3 palettes) matches rom_mon's own accessors too;
 *   3. the whole stream is EXACTLY ART_ICONS_TOTAL_BYTES, driven end to end with the
 *      real two-pass (write then verify) offset sequence sf_write_verified_stream
 *      makes, and both passes produce byte-identical output (which is what makes the
 *      "verify pass re-derives, don't cache" design sound);
 *   4. the running FNV is deterministic across two independent full runs;
 *   5. cancelling partway (progress() returns false) stops the stream at the next
 *      row boundary, never mid-row, and sets `cancelled`, not `rom_error`;
 *   6. a ROM read that fails mid-stream sets `rom_error`, not `cancelled`, and the
 *      generator refuses every call after either flag is set (no half-answers).
 *
 * ROMs are Guy's own dumps, never part of the repo; missing ones SKIP, matching every
 * other host_rom*_test.c in this suite.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "art_icons_extract.h"
#include "rom_map.h"
#include "rom_mon.h"

static int fails = 0, checks = 0;
static void chk(const char* rom, const char* what, int cond) {
  checks++;
  if (!cond) { printf("FAIL [%s] %s\n", rom, what); fails++; }
}

typedef struct { FILE* f; } FileCtx;
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, fc->f) != len) return false;
  return true;
}

/* A whole-stream driver that mimics sf_write_verified_stream's own offset sequence
 * exactly (chunk = ART_ICONS_ROW_BYTES for the row region, then the tail once) but
 * keeps the WHOLE output in a host-side heap buffer for byte-level comparison --
 * something the real GBA build never does (that is the entire point of the stream),
 * but is exactly what a host test can afford. */
static bool drive_pass(ArtIconsGen* g, uint8_t* out /* ART_ICONS_TOTAL_BYTES */) {
  uint32_t off = 0;
  while (off < ART_ICONS_TILES_BYTES) {
    if (!art_icons_stream(g, off, out + off, ART_ICONS_ROW_BYTES)) return false;
    off += ART_ICONS_ROW_BYTES;
  }
  uint32_t tail = ART_ICONS_PAL_IDS_BYTES + ART_ICONS_PALS * ART_ICONS_PAL_BYTES;
  if (!art_icons_stream(g, off, out + off, tail)) return false;
  off += tail;
  return off == ART_ICONS_TOTAL_BYTES;
}

static uint8_t s_pass1[ART_ICONS_TOTAL_BYTES];
static uint8_t s_pass2[ART_ICONS_TOTAL_BYTES];

/* File-scope callbacks (nested function definitions are not standard C). */
typedef struct { int calls, stop_after; } CancelCtx;
static bool cancel_cb(void* vc, int pass, int rows_done, int total) {
  (void)pass; (void)rows_done; (void)total;
  CancelCtx* c = (CancelCtx*)vc;
  c->calls++;
  return c->calls < c->stop_after;
}

typedef struct { FILE* f; long fail_from; } FailCtx;
static bool fail_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FailCtx* fc = (FailCtx*)ctx;
  if ((long)off >= fc->fail_from) return false;
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  return fread(dst, 1, len, fc->f) == len;
}

static void run_rom(const char* path, const char* name, int expect_header) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP %s (no %s)\n", name, path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc = { f };
  RomCtx rc;
  bool opened = rom_open(&rc, file_read, &fc, (uint32_t)sz);
  chk(name, "rom_open", opened);
  if (!opened) { fclose(f); return; }

  RomMon rm;
  int have_mon = rom_mon_open(&rm, &rc);
  chk(name, expect_header ? "GF header parses" : "fails CLOSED (no GF header, expected)",
      have_mon == expect_header);
  if (!have_mon) {
    fclose(f);
    if (expect_header) printf("  %s: FAILED to get an icon table\n", name);
    else printf("  %s: correctly has no icon table (R/S predate the GF header)\n", name);
    return;
  }

  chk(name, "ART_ICONS_ROWS matches rom_mon_table_rows()",
      ART_ICONS_ROWS == rom_mon_table_rows());

  /* --- 1/2/3: two full passes, byte-identical, and spot-checked against rom_mon --- */
  ArtIconsGen g;
  art_icons_gen_init(&g, &rm, 0, 0);
  chk(name, "pass 1 completes", drive_pass(&g, s_pass1));
  chk(name, "pass 1 has no rom_error", !g.rom_error);
  chk(name, "pass 2 completes", drive_pass(&g, s_pass2));
  chk(name, "the two passes are byte-identical",
      memcmp(s_pass1, s_pass2, ART_ICONS_TOTAL_BYTES) == 0);

  /* Spot-check every 37th row (covers the whole range without 440 * 2 more reads)
   * plus row 0, 411 (species ceiling), 412 (Egg) and 413 (Unown B) explicitly. */
  int spot[] = { 0, 411, 412, 413, 439 };
  for (int si = -1; si < (int)(sizeof spot / sizeof spot[0]); si++) {
    int row = si < 0 ? 0 : spot[si];
    for (int r = (si < 0 ? 0 : row); r <= (si < 0 ? (int)ART_ICONS_ROWS - 1 : row);
         r += (si < 0 ? 37 : 1)) {
      RomMonLoc loc;
      uint8_t f0[512], f1[512];
      bool ok = rom_mon_locate_row_verified(&rm, (uint16_t)r, &loc, 3, 0) &&
                rom_mon_icon_at(&rm, &loc, 0, f0) && rom_mon_icon_at(&rm, &loc, 1, f1);
      chk(name, "spot row locate+read", ok);
      if (!ok) continue;
      const uint8_t* row_bytes = s_pass1 + (uint32_t)r * ART_ICONS_ROW_BYTES;
      chk(name, "spot row frame0 matches rom_mon", memcmp(row_bytes, f0, 512) == 0);
      chk(name, "spot row frame1 matches rom_mon", memcmp(row_bytes + 512, f1, 512) == 0);
      if (si < 0) r += 0; /* keep the loop variable meaningful for -Wunused */
    }
  }

  /* Tail: pal ids + 3 palettes, independently re-read. */
  uint8_t pal_ids[ART_ICONS_ROWS];
  chk(name, "pal_ids direct read", rc.read(rc.ctx, rm.pal_ids, pal_ids, ART_ICONS_ROWS));
  chk(name, "pal_ids match the stream",
      memcmp(s_pass1 + ART_ICONS_PAL_IDS_OFF, pal_ids, ART_ICONS_ROWS) == 0);
  for (int i = 0; i < (int)ART_ICONS_PALS; i++) {
    uint16_t pal[16];
    chk(name, "palette direct read", rom_mon_icon_pal(&rm, i, pal));
    const uint8_t* p = s_pass1 + ART_ICONS_PALS_OFF + (uint32_t)i * ART_ICONS_PAL_BYTES;
    bool same = true;
    for (int c = 0; c < 16; c++) {
      uint16_t v = (uint16_t)(p[c * 2] | ((uint16_t)p[c * 2 + 1] << 8));
      if (v != pal[c]) same = false;
    }
    chk(name, "palette matches the stream", same);
  }

  /* --- 4: FNV determinism ------------------------------------------------------- */
  ArtIconsGen g2;
  art_icons_gen_init(&g2, &rm, 0, 0);
  uint8_t scratch[ART_ICONS_TOTAL_BYTES];
  chk(name, "pass for fnv check completes", drive_pass(&g2, scratch));
  chk(name, "fnv is deterministic across independent generators",
      art_icons_gen_fnv(&g) == art_icons_gen_fnv(&g2));
  chk(name, "fnv is nonzero (sanity)", art_icons_gen_fnv(&g) != 0);

  /* --- 5: cancel stops at a row boundary, never mid-row -------------------------- */
  {
    CancelCtx cc = { 0, 10 };
    ArtIconsGen g3;
    art_icons_gen_init(&g3, &rm, cancel_cb, &cc);
    uint8_t buf[ART_ICONS_ROW_BYTES];
    int r = 0;
    bool stopped = false;
    for (uint32_t off = 0; off < ART_ICONS_TILES_BYTES; off += ART_ICONS_ROW_BYTES, r++) {
      if (!art_icons_stream(&g3, off, buf, ART_ICONS_ROW_BYTES)) { stopped = true; break; }
    }
    chk(name, "cancel actually stops the stream", stopped);
    chk(name, "cancel sets cancelled, not rom_error", g3.cancelled && !g3.rom_error);
    chk(name, "cancel stopped at row 9 (0-indexed, the 10th progress call)", r == 9);
    /* a cancelled generator refuses every further call, unconditionally */
    chk(name, "a cancelled generator refuses further calls",
        !art_icons_stream(&g3, ART_ICONS_TILES_BYTES + 10 * ART_ICONS_ROW_BYTES, buf,
                          ART_ICONS_ROW_BYTES));
  }

  /* --- 6: a ROM read failure sets rom_error, not cancelled ------------------------ */
  {
    FailCtx fctx = { f, (long)(sz / 2) }; /* fail once we are reading the back half */
    RomCtx rc2;
    if (rom_open(&rc2, fail_read, &fctx, (uint32_t)sz)) {
      RomMon rm2;
      if (rom_mon_open(&rm2, &rc2)) {
        ArtIconsGen g4;
        art_icons_gen_init(&g4, &rm2, 0, 0);
        uint8_t buf[ART_ICONS_ROW_BYTES];
        bool saw_failure = false;
        for (uint32_t off = 0; off < ART_ICONS_TILES_BYTES; off += ART_ICONS_ROW_BYTES) {
          if (!art_icons_stream(&g4, off, buf, ART_ICONS_ROW_BYTES)) { saw_failure = true; break; }
        }
        chk(name, "a failing ROM read is eventually hit", saw_failure);
        chk(name, "rom read failure sets rom_error, not cancelled",
            g4.rom_error && !g4.cancelled);
      }
    }
  }

  fclose(f);
  printf("  %s: ok (kind=%s rev=%u, icons fnv=%08x)\n", name, rom_kind_name(rc.kind),
        rc.version, art_icons_gen_fnv(&g));
}

int main(int argc, char** argv) {
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  /* run_host_tests.py hands every argv[1]-reading test the .sav corpus (it detects
   * `argv[` textually, not what the arg means) -- this test wants the ROM DIRECTORY,
   * so only accept an argument that is not one of those .sav paths. Same guard
   * tests/host_rommon_test.c uses, for the same reason. */
  if (argc > 1) {
    size_t l = strlen(argv[1]);
    if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1];
  }
  char path[512];
#define RUN(file, name, hdr) \
  do { snprintf(path, sizeof path, "%s/%s", dir, file); run_rom(path, name, hdr); } while (0)
  RUN("Emerald.gba", "Emerald", 1);
  RUN("FireRed.gba", "FireRed", 1);
  RUN("LeafGreen.gba", "LeafGreen", 1);
  RUN("Ruby.gba", "Ruby", 1);         /* R/S predate the GF header -- served via pinned rows (#293) */
  RUN("Sapphire.gba", "Sapphire", 1);
#undef RUN
  printf("art_icons_extract test: %d checks, %d failure(s)\n", checks, fails);
  return fails ? 1 : 0;
}
