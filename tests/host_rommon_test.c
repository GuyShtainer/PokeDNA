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

/* The reader can inject a cart-bus/EZ-Flash style GARBLE: reads at `g_off` come back
 * with byte 0 flipped by +0x10, which for an icon-table pointer is a DIFFERENT but
 * still perfectly in-bounds address — exactly the failure ptr_ok cannot see.
 * g_every2 garbles only the first read of each back-to-back pair (a glitch that keeps
 * hitting one pass), so the verify can never agree; without it the garble is spent
 * after `g_left` reads and a retry heals. */
typedef struct {
  FILE* f; int reads;
  uint32_t g_off; int g_left; int g_every2; int g_hits;
} FileCtx;
static bool file_read(void* ctx, uint32_t off, void* dst, uint32_t len) {
  FileCtx* fc = (FileCtx*)ctx;
  fc->reads++;                          /* transaction count: what the SD path pays for */
  if (fseek(fc->f, (long)off, SEEK_SET) != 0) return false;
  if (fread(dst, 1, len, fc->f) != len) return false;
  if (fc->g_left > 0 && off == fc->g_off) {
    int hit = fc->g_hits++;
    if (!fc->g_every2 || (hit % 2) == 0) { fc->g_left--; ((uint8_t*)dst)[0] ^= 0x10; }
  }
  return true;
}
static void garble(FileCtx* fc, uint32_t off, int n, int every2) {
  fc->g_off = off; fc->g_left = n; fc->g_every2 = every2; fc->g_hits = 0;
}
static void garble_off(FileCtx* fc) { fc->g_left = 0; fc->g_every2 = 0; fc->g_hits = 0; }

static int blank(const uint8_t* p, int n) {
  for (int i = 0; i < n; i++) if (p[i]) return 0;
  return 1;
}

static void run_rom(const char* path, const char* name, int expect_header) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("SKIP %s (no %s)\n", name, path); return; }
  fseek(f, 0, SEEK_END); long sz = ftell(f);
  FileCtx fc = { f, 0, 0, 0, 0, 0 };
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

  /* 6) the locate/read SPLIT (box_oam streams icons off the SD, and its double-read
   *    verify used to redo both table lookups per pass — six reads per icon). The split
   *    must produce the SAME bytes and the same palette, and it must cost fewer reads. */
  {
    uint8_t wf[ROM_MON_ICON_BYTES], sf[ROM_MON_ICON_BYTES];
    uint8_t wp = 0xFF;
    fc.reads = 0;
    int w_ok = rom_mon_icon(&rm, 25, 0, 0, wf, &wp);   /* Pikachu, the wrapper */
    int w_reads = fc.reads;

    RomMonLoc loc;
    fc.reads = 0;
    int l_ok = rom_mon_locate(&rm, 25, 0, &loc);
    int l_reads = fc.reads;
    fc.reads = 0;
    int r_ok = rom_mon_icon_at(&rm, &loc, 0, sf);
    int r_reads = fc.reads;

    chk(name, "locate + icon_at both succeed", w_ok && l_ok && r_ok);
    chk(name, "split frame == wrapper frame", memcmp(wf, sf, sizeof wf) == 0);
    chk(name, "located palette == wrapper palette", loc.ok && loc.pal == wp);
    chk(name, "locate costs 2 reads, icon_at costs 1", l_reads == 2 && r_reads == 1);
    chk(name, "one verified icon: 2 reads via the split, not 6",
        l_reads + 2 * r_reads == 4 && 2 * w_reads == 6 && w_reads == l_reads + r_reads);

    /* frame 1 off the SAME location: no further lookups */
    uint8_t s1[ROM_MON_ICON_BYTES];
    fc.reads = 0;
    chk(name, "frame 1 from a cached location", rom_mon_icon_at(&rm, &loc, 1, s1) && fc.reads == 1);
    chk(name, "the two cached frames differ", memcmp(sf, s1, sizeof sf) != 0);

    /* fail-closed contract the box_oam memo relies on */
    RomMonLoc bad_loc;
    chk(name, "out-of-range species leaves ok = 0",
        !rom_mon_locate(&rm, 440, 0, &bad_loc) && !bad_loc.ok);
    chk(name, "out-of-range Unown form leaves ok = 0",
        !rom_mon_locate(&rm, 201, 28, &bad_loc) && !bad_loc.ok);
    chk(name, "icon_at rejects a never-located loc", !rom_mon_icon_at(&rm, &bad_loc, 0, sf));
    chk(name, "icon_at rejects frame 2", !rom_mon_icon_at(&rm, &loc, 2, sf));
  }

  /* 7) the VERIFIED locate. Memoising the location makes the frame's double-read verify
   *    BLIND to a garbled lookup — both of its passes then read the same wrong offset,
   *    agree, and paint another species' icon. So the lookup gets its own double read. */
  {
    RomMonLoc good, loc;
    uint32_t p_off = rm.icons + 25 * 4;              /* Pikachu's icon-table pointer */
    garble_off(&fc);
    chk(name, "clean locate for the garble baseline", rom_mon_locate(&rm, 25, 0, &good));

    /* the exposure itself: one garbled pointer read and the PLAIN locate says yes */
    garble(&fc, p_off, 1, 0);
    int blind = rom_mon_locate(&rm, 25, 0, &loc);
    garble_off(&fc);
    chk(name, "a garbled pointer passes the PLAIN locate (the exposure)",
        blind && loc.ok && loc.tiles != good.tiles);

    /* a glitch that keeps hitting the first pass: never agrees -> fail closed + flagged */
    int unstable = -1;
    garble(&fc, p_off, 99, 1);
    int vr = rom_mon_locate_verified(&rm, 25, 0, &loc, 4, &unstable);
    garble_off(&fc);
    chk(name, "verified locate rejects a lookup it cannot agree on",
        !vr && !loc.ok && unstable == 1);

    /* one-shot glitch: the retry heals it and lands on the TRUE location */
    unstable = -1;
    garble(&fc, p_off, 1, 0);
    vr = rom_mon_locate_verified(&rm, 25, 0, &loc, 4, &unstable);
    garble_off(&fc);
    chk(name, "verified locate heals a transient garble",
        vr && loc.ok && loc.tiles == good.tiles && loc.pal == good.pal && unstable == 0);

    /* the palette byte has the same exposure and the same cover */
    unstable = -1;
    garble(&fc, rm.pal_ids + 25, 99, 1);
    vr = rom_mon_locate_verified(&rm, 25, 0, &loc, 4, &unstable);
    garble_off(&fc);
    chk(name, "verified locate covers the palette byte too",
        !vr && !loc.ok && unstable == 1);

    /* cost: two reads per field and NOT a third visit to either table */
    unstable = -1; fc.reads = 0;
    vr = rom_mon_locate_verified(&rm, 25, 0, &loc, 4, &unstable);
    chk(name, "a clean verified locate costs 4 reads (2 per field)",
        vr && fc.reads == 4 && unstable == 0);
    chk(name, "verified == plain on a clean read",
        loc.tiles == good.tiles && loc.pal == good.pal);

    /* a bounds failure must NOT read as unstable, or a corrupt species id in the save
     * would log on every box load */
    unstable = -1;
    chk(name, "a bounds failure is not flagged unstable",
        !rom_mon_locate_verified(&rm, 440, 0, &loc, 4, &unstable) && unstable == 0);
    /* attempts is clamped up: nobody can ask for an unverified "verified" locate */
    unstable = -1; fc.reads = 0;
    chk(name, "attempts < 2 still verifies",
        rom_mon_locate_verified(&rm, 25, 0, &loc, 0, &unstable) && loc.ok && fc.reads == 4);
  }

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
