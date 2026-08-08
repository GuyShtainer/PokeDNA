/* Host (PC) test for rom_mon — Pokemon icons read from REAL retail ROMs. Because
 * rom_mon does all its I/O through the RomCtx callback, the code under test is
 * byte-for-byte the code that runs on the GBA (fused today, FatFs later).
 *
 * What it proves:
 *   1) the GF header parses on Emerald + FireRed + LeafGreen, and rom_mon FAILS
 *      CLOSED on Ruby/Sapphire (no header — they need pinned rows, not guesses);
 *   2) every one of the 440 icon-table entries (386 species + ceiling gap + Egg +
 *      27 Unown letters) yields an in-bounds picture pointer, a palette id 0..2,
 *      and a non-blank 512 B frame;
 *   3) both frames differ for a known animated icon (the 2-frame bob is real);
 *   4) the Unown letter axis maps where it should (form 1 = table entry 413) and
 *      the Egg icon (412) reads;
 *   5) the three shared palettes read and are non-degenerate.
 *
 * ROMs are the user's own dumps, never part of the repo; missing ROMs SKIP.
 *
 * Build + run (from the repo root):
 *   cc -std=c11 -I source tests/host_rommon_test.c source/rom_mon.c source/rom_map.c -o /tmp/hrmn && /tmp/hrmn
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
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
  return fread(dst, 1, len, fc->f) == len;
}

static int blank(const uint8_t* p, int n) {
  for (int i = 0; i < n; i++) if (p[i]) return 0;
  return 1;
}

static void run_rom(const char* path, const char* name, int expect_header) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP %s (no %s)\n", name, path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc = { f };
  RomCtx rc;
  if (!rom_open(&rc, file_read, &fc, (uint32_t)sz)) {
    chk(name, "rom_open accepts the retail dump", 0);
    fclose(f); return;
  }

  RomMon rm;
  int ok = rom_mon_open(&rm, &rc);
  chk(name, expect_header ? "GF header parses" : "fails CLOSED (no GF header)",
      ok == expect_header);
  if (!ok) { fclose(f); return; }

  /* 2) every table entry reads sanely */
  int bad = 0, blanks = 0;
  uint8_t frame[ROM_MON_ICON_BYTES]; uint8_t pal;
  for (uint16_t ts = 0; ts < 440; ts++) {
    uint16_t sp = ts; uint8_t form = 0;
    if (ts >= 413) { sp = 201; form = (uint8_t)(ts - 413 + 1); }   /* Unown B.. */
    if (!rom_mon_icon(&rm, sp, form, 0, frame, &pal)) { bad++; continue; }
    if (blank(frame, sizeof frame)) blanks++;
  }
  chk(name, "all 440 icon entries read", bad == 0);
  chk(name, "no icon frame is blank", blanks == 0);
  if (bad || blanks) printf("  [%s] bad=%d blanks=%d\n", name, bad, blanks);

  /* 3) the 2-frame bob is real data (Pikachu, internal 25) */
  uint8_t f0[ROM_MON_ICON_BYTES], f1[ROM_MON_ICON_BYTES];
  chk(name, "Pikachu frame 0 reads", rom_mon_icon(&rm, 25, 0, 0, f0, 0));
  chk(name, "Pikachu frame 1 reads", rom_mon_icon(&rm, 25, 0, 1, f1, 0));
  chk(name, "the two frames differ", memcmp(f0, f1, sizeof f0) != 0);

  /* 4) Egg + Unown letters */
  chk(name, "the Egg icon (412) reads", rom_mon_icon(&rm, 412, 0, 0, frame, &pal));
  uint8_t ua[ROM_MON_ICON_BYTES], ub[ROM_MON_ICON_BYTES];
  chk(name, "Unown A (species 201) reads", rom_mon_icon(&rm, 201, 0, 0, ua, 0));
  chk(name, "Unown B (form 1) reads",      rom_mon_icon(&rm, 201, 1, 0, ub, 0));
  chk(name, "Unown A != Unown B",          memcmp(ua, ub, sizeof ua) != 0);
  chk(name, "out-of-range form rejected",  !rom_mon_icon(&rm, 201, 28, 0, ua, 0));
  chk(name, "out-of-range species rejected", !rom_mon_icon(&rm, 440, 0, 0, ua, 0));

  /* 5) the three shared palettes */
  for (int p = 0; p < ROM_MON_PALS; p++) {
    uint16_t pd[16];
    char what[40]; sprintf(what, "palette %d reads + non-degenerate", p);
    int r = rom_mon_icon_pal(&rm, p, pd);
    int distinct = 0;
    if (r) for (int i = 1; i < 16; i++) if (pd[i] != pd[0]) { distinct = 1; break; }
    chk(name, what, r && distinct);
  }
  chk(name, "palette 3 rejected", !rom_mon_icon_pal(&rm, 3, 0));

  printf("  %s: ok (kind=%s rev=%u)\n", name, rom_kind_name(rc.kind), rc.version);
  fclose(f);
}

int main(int argc, char** argv) {
  const char* dir = "/Users/guyshtainer/VSCodeProjects/gba-toolkit/roms";
  /* run_host_tests.py hands every argv[1]-reading test the .sav corpus — this test
   * wants the ROM DIRECTORY, so only accept an argument that is one. */
  if (argc > 1) { size_t l = strlen(argv[1]); if (l < 4 || strcmp(argv[1] + l - 4, ".sav") != 0) dir = argv[1]; }
  char p[512];
  sprintf(p, "%s/Emerald.gba", dir);   run_rom(p, "Emerald",   1);
  sprintf(p, "%s/FireRed.gba", dir);   run_rom(p, "FireRed",   1);
  sprintf(p, "%s/LeafGreen.gba", dir); run_rom(p, "LeafGreen", 1);
  sprintf(p, "%s/Ruby.gba", dir);      run_rom(p, "Ruby",      0);
  sprintf(p, "%s/Sapphire.gba", dir);  run_rom(p, "Sapphire",  0);
  printf("rom_mon test: %d checks, %d failure(s)\n", checks, fails);
  return fails ? 1 : 0;
}
